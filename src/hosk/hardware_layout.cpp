/*
 * hardware_layout.cpp: Defines the parsing of local hardware information
 *
 *  Author: Henry Daly, 2018
 */

#include "hardware_layout.h"

const char* hyperthread_enabled  = "lscpu | grep ht";
const char* socket_info          = "lscpu | egrep 'Socket' | awk '{print $NF}'";
const char* thread_info          = "lscpu | egrep 'Thread' | awk '{print $NF}'";
const char* core_info            = "lscpu | egrep '^Core' | awk '{print $NF}'";
const char* num_cpu_info         = "lscpu | egrep '^CPU\\(s\\)' | awk '{print $NF}'";

std::string pipe_to_string(const char* command) {
    FILE* file = popen( command, "r" ) ;
    if(file) {
        std::ostringstream stm;
        constexpr std::size_t MAX_LINE_SZ = 1024;
        char line[MAX_LINE_SZ];
        while(fgets(line, MAX_LINE_SZ, file)){ stm << line << '\n'; }
        pclose(file) ;
        return stm.str();
    }
    return "";
}

hl_t* get_hardware_layout(void) {
   hl_t* machine = (hl_t*)malloc(sizeof(hl_t));

   // Ensure HyperThreading is enabled (i.e. 2 hardware threads per core)
   std::string ht_enabled = pipe_to_string(hyperthread_enabled);
   int threads_per_core  = std::stoi(pipe_to_string(thread_info));
   if(!ht_enabled.compare("") || threads_per_core != THREADS_PER_CORE) {
      std::cout << "ERROR: HyperThreading not enabled!\n";
      exit(-1);
   }

   // Get Hardware information and create structure
   int socket_num       = std::stoi(pipe_to_string(socket_info));
   int cores_per_socket = std::stoi(pipe_to_string(core_info));
   machine->max_cpu_num = std::stoi(pipe_to_string(num_cpu_info));
   int num_hw_cpus = socket_num * cores_per_socket * threads_per_core;
   if(num_hw_cpus != machine->max_cpu_num) {
      std::cout << "ERROR: lscpu parse issue - CPU numbers don't match!\n";
      exit(-1);
   }
   machine->num_sockets = socket_num;
   machine->cores_per_socket = cores_per_socket;
   machine->sockets = (socket_t*)malloc(socket_num * sizeof(socket_t));
   for(int i=0; i < socket_num; ++i) {
      machine->sockets[i].cores = (core_t*)malloc(cores_per_socket * sizeof(core_t));
   }

   // Initialize hardware layout
   int csocket = 0;
   int ccore = 0;
   int cphysical = 0;
   for(int pnum = 0; pnum < num_hw_cpus; ++pnum) {
      machine->sockets[csocket].cores[ccore].hwthread_id[cphysical] = pnum;
      ccore++;
      if(ccore % cores_per_socket == 0) {
         ccore = 0;
         csocket++;
         if(csocket % socket_num == 0) {
            csocket = 0;
            cphysical++;
         }
      }
      if((cphysical == THREADS_PER_CORE) && (pnum + 1 < num_hw_cpus)) {
         std::cout << "ERROR: More CPUs than calculated!\n";
         exit(-1);
      }
   }
   return machine;
}

void free_hardware_layout(hl_t* m) {
   for(int i = 0; i < m->num_sockets; ++i) {
      free(m->sockets[i].cores);
   }
   free(m->sockets);
   free(m);
}

void print_hardware_layout(hl_t* m) {
   using std::cout;
   cout << "Sockets:          " << m->num_sockets        << "\n";
   cout << "Cores/Socket:     " << m->cores_per_socket   << "\n";
   cout << "Hardware Threads: " << m->max_cpu_num        << "\n";
   for(int i = 0; i < m->num_sockets; ++i) {
      cout << "Socket " << i << ":\n";
      socket_t msocket = m->sockets[i];
      for(int j = 0; j < m->cores_per_socket; ++j) {
         core_t mcore = msocket.cores[j];
         cout << "  Core " << j << ": T1= " << mcore.hwthread_id[0] << ",\tT2= " << mcore.hwthread_id[1] << "\n";
      }
   }
}
