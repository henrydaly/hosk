/*
 * test.cpp: application test file
 *
 *
 * Built off No Hotspot Skip List test.c (2013)
 *
 * Synchrobench is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <assert.h>
#include <atomic_ops.h>
#include <getopt.h>
#include <limits.h>
#include <numa.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "allocator.h"
#include "common.h"
#include "enclave.h"
#include "skiplist.h"

#define DEFAULT_DURATION               10000
#define DEFAULT_INITIAL                1024
#define DEFAULT_NB_THREADS             1
#define DEFAULT_RANGE                  0x7FFFFFFF
#define DEFAULT_SEED                   0
#define DEFAULT_UPDATE                 20
#define DEFAULT_ALTERNATE              0
#define DEFAULT_EFFECTIVE              1
#define DEFAULT_UNBALANCED             0
#define DEFAULT_UPDATE_FREQUENCY       20
#define MAX_NUMA_ZONES                 numa_max_node() + 1
#define MIN_NUMA_ZONES                 1
#define XSTR(s)                        STR(s)
#define STR(s)                         #s
#define ATOMIC_CAS_MB(a, e, v)         (AO_compare_and_swap_full((VOLATILE AO_t *)(a), (AO_t)(e), (AO_t)(v)))
#define ATOMIC_FETCH_AND_INC_FULL(a)   (AO_fetch_and_add1_full((VOLATILE AO_t *)(a)))
#define VAL_MIN                        INT_MIN
#define VAL_MAX                        INT_MAX

struct thread_init_args {
   int      cpu_num;
   node_t*  node_sentinel;
   unsigned allocator_size;
   uint     freq;
};

int num_numa_zones = MAX_NUMA_ZONES;
VOLATILE AO_t stop;
unsigned int global_seed;
pthread_key_t rng_seed_key;
unsigned int levelmax;
enclave** enclaves;
extern numa_allocator** allocators;

void barrier_init(barrier_t *b, int n) {
   pthread_cond_init(&b->complete, NULL);
   pthread_mutex_init(&b->mutex, NULL);
   b->count = n;
   b->crossing = 0;
}

void barrier_cross(barrier_t *b) {
   pthread_mutex_lock(&b->mutex);
   /* One more thread through */
   b->crossing++;
   /* If not all here, wait */
   if (b->crossing < b->count) {
      pthread_cond_wait(&b->complete, &b->mutex);
   } else {
      pthread_cond_broadcast(&b->complete);
      /* Reset for next time */
      b->crossing = 0;
   }
   pthread_mutex_unlock(&b->mutex);
}

int floor_log_2(unsigned int n) {
   int pos = 0;
   if (n >= 1<<16) { n >>= 16; pos += 16; }
   if (n >= 1<< 8) { n >>=  8; pos +=  8; }
   if (n >= 1<< 4) { n >>=  4; pos +=  4; }
   if (n >= 1<< 2) { n >>=  2; pos +=  2; }
   if (n >= 1<< 1) {           pos +=  1; }
   return ((n == 0) ? (-1) : pos);
}

/* thread_init() - initializes the enclave object for a thread */
void* thread_init(void* args) {
   int buffer_size = 1000;
   thread_init_args* zia = (thread_init_args*)args;
   int numa_zone = zia->cpu_num % num_numa_zones;
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(zia->cpu_num, &cpuset);
   sleep(1);
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
   numa_set_preferred(numa_zone);
   numa_allocator* na = new numa_allocator(zia->allocator_size);
   allocators[zia->cpu_num] = na;
   mnode_t* mnode = mnode_new(NULL, zia->node_sentinel, 1, numa_zone);
   inode_t* inode = inode_new(NULL, NULL, mnode, numa_zone);
   enclave* en = new enclave(buffer_size, zia->cpu_num, numa_zone, inode, zia->freq);
   enclaves[zia->cpu_num] = en;
   return NULL;
}

void catcher(int sig) { printf("CAUGHT SIGNAL %d\n", sig); }

int main(int argc, char **argv) {
   if(numa_available() == -1) {
      printf("Error: NUMA unavailable on this system.\n");
      exit(0);
   }
   struct option long_options[] = {
      // These options don't set a flag
      {"help",                      no_argument,       NULL, 'h'},
      {"duration",                  required_argument, NULL, 'd'},
      {"initial-size",              required_argument, NULL, 'i'},
      {"num-threads",               required_argument, NULL, 'n'},
      {"range",                     required_argument, NULL, 'r'},
      {"seed",                      required_argument, NULL, 's'},
      {"update-rate",               required_argument, NULL, 'u'},
      {"elasticity",                required_argument, NULL, 'x'},
      {NULL, 0, NULL, 0}
   };

   int i, c, size;
   unsigned int val = 0;
   unsigned long adds = 0, removes = 0;
   unsigned long reads = 0, effreads = 0, updates = 0, effupds = 0;
   pthread_t *threads;
   pthread_attr_t attr;
   barrier_t barrier;

   app_param* data;
   struct timeval start, end;
   struct timespec timeout;
   int duration = DEFAULT_DURATION;
   int initial = DEFAULT_INITIAL;
   int nb_threads = DEFAULT_NB_THREADS;
   long range = DEFAULT_RANGE;
   int seed = DEFAULT_SEED;
   int update = DEFAULT_UPDATE;
   int alternate = DEFAULT_ALTERNATE;
   int effective = DEFAULT_EFFECTIVE;
   uint update_frequency = DEFAULT_UPDATE_FREQUENCY;
   sigset_t block_set;
   struct sl_node *temp;
   int unbalanced = DEFAULT_UNBALANCED;
   while(1) {
      i = 0;
      c = getopt_long(argc, argv, "hAf:d:i:t:r:S:u:U:z:P:y:", long_options, &i);

      if(c == -1) break;

      if(c == 0 && long_options[i].flag == 0) { c = long_options[i].val; }

      switch(c) {
         case 0: break;
         case 'h':
            printf("intset -- STM stress test "
                   "(skip list)\n"
                   "\n"
                   "Usage:\n"
                   "  intset [options...]\n"
                   "\n"
                   "Options:\n"
                   "  -h, --help\n"
                   "        Print this message\n"
                   "  -A, --Alternate\n"
                   "        Consecutive insert/remove target the same value\n"
                   "  -f, --effective <int>\n"
                   "        update txs must effectively write (0=trial, 1=effective, default=" XSTR(DEFAULT_EFFECTIVE) ")\n"
                   "  -d, --duration <int>\n"
                   "        Test duration in milliseconds (0=infinite, default=" XSTR(DEFAULT_DURATION) ")\n"
                   "  -i, --initial-size <int>\n"
                   "        Number of elements to insert before test (default=" XSTR(DEFAULT_INITIAL) ")\n"
                   "  -t, --thread-num <int>\n"
                   "        Number of threads (default=" XSTR(DEFAULT_NB_THREADS) ")\n"
                   "  -r, --range <int>\n"
                   "        Range of integer values inserted in set (default=" XSTR(DEFAULT_RANGE) ")\n"
                   "  -S, --seed <int>\n"
                   "        RNG seed (0=time-based, default=" XSTR(DEFAULT_SEED) ")\n"
                   "  -u, --update-rate <int>\n"
                   "        Percentage of update transactions (default=" XSTR(DEFAULT_UPDATE) ")\n"
                   "  -z <int>\n"
                   "        Number of NUMA zones to use (default = " XSTR(MAX_NUMA_ZONES) ")\n"
                   "  -y <int>\n"
                   "        Frequency of index layer updates"
                   );
            exit(0);
         case 'A':
            alternate = 1;
            break;
         case 'f':
            effective = atoi(optarg);
            break;
         case 'd':
            duration = atoi(optarg);
            break;
         case 'i':
            initial = atoi(optarg);
            break;
         case 't':
            nb_threads = atoi(optarg);
            break;
         case 'r':
            range = atol(optarg);
            break;
         case 'S':
            seed = atoi(optarg);
            break;
         case 'u':
            update = atoi(optarg);
            break;
         case 'U':
            unbalanced = atoi(optarg);
            break;
         case 'z':
            num_numa_zones = atoi(optarg);
            break;
         case 'y':
            update_frequency = atoi(optarg);
            break;
         case '?':
            printf("Use -h or --help for help\n");
            exit(0);
         default: exit(1);
      }
   }

   assert(duration >= 0);
   assert(initial >= 0);
   assert(nb_threads > 1);
   assert(range > 0 && range >= initial);
   assert(update >= 0 && update <= 100);
   assert(num_numa_zones >= MIN_NUMA_ZONES && num_numa_zones <= MAX_NUMA_ZONES);
   if(num_numa_zones > nb_threads) { num_numa_zones = nb_threads; }  // don't spawn unnecessary background threads


   printf("Set type     : skip list\n");
   printf("Duration     : %d\n", duration);
   printf("Initial size : %u\n", initial);
   printf("Nb threads   : %d\n", nb_threads);
   printf("Value range  : %ld\n", range);
   printf("Seed         : %d\n", seed);
   printf("Update rate  : %d\n", update);
   printf("Alternate    : %d\n", alternate);
   printf("Effective   : %d\n", effective);
   printf("Type sizes   : int=%d/long=%d/ptr=%d/word=%d\n", (int)sizeof(int), (int)sizeof(long), (int)sizeof(void *), (int)sizeof(uintptr_t));
   printf("NUMA Zones   : %d\n", num_numa_zones);
   printf("Update freq  : %d\n", update_frequency);

   timeout.tv_sec = duration / 1000;
   timeout.tv_nsec = (duration % 1000) * 1000000;

   if((data = (app_param*)malloc(nb_threads * sizeof(app_param))) == NULL) {
      perror("malloc");
      exit(1);
   }
   if((threads = (pthread_t *)malloc(nb_threads * sizeof(pthread_t))) == NULL) {
      perror("malloc");
      exit(1);
   }

   if (seed == 0) { srand((int)time(0)); }
   else           { srand(seed); }
   levelmax = floor_log_2((unsigned int) initial);

   // create sentinel node on NUMA zone 0
   node_t* sentinel_node = node_new(0, NULL, NULL, NULL);
   // HOSK setup
   enclaves = (enclave**)malloc(nb_threads*sizeof(enclave*));
   pthread_t* thds = (pthread_t*)malloc(nb_threads*sizeof(pthread_t));
   allocators = (numa_allocator**)malloc(nb_threads*sizeof(numa_allocator*));
   unsigned num_expected_nodes = (unsigned)((2 * initial * (1.0 + (update/100.0))) / nb_threads);
   unsigned buffer_size = CACHE_LINE_SIZE * num_expected_nodes;

   thread_init_args* zia = (thread_init_args*)malloc(sizeof(thread_init_args));
   zia->node_sentinel = sentinel_node;
   zia->allocator_size = buffer_size;
   for(int i = 0; i < nb_threads; ++i) {
      zia->cpu_num = i;
      pthread_create(&thds[i], NULL, thread_init, (void*)zia);
   }

   for(int i = 0; i < num_numa_zones; ++i) {
      void *retval;
      pthread_join(thds[i], &retval);
   }
   stop = 0;
   global_seed = rand();
   if (pthread_key_create(&rng_seed_key, NULL) != 0) {
      fprintf(stderr, "Error creating thread local\n");
      exit(1);
   }
   pthread_setspecific(rng_seed_key, &global_seed);

   // Initial skip list population
   printf("Adding %d entries to set\n", initial);
   for(int i = 0; i < nb_threads; ++i) {
      enclaves[i]->start_helper(0);
   }
   int add_nodes, successfully_added = 0;
   uint last = 0;
   int d = initial / nb_threads;
   int m = initial % nb_threads;
   for(int j = 0; j < nb_threads; j++) {
      // if size !divide across threads -> first m threads get + 1
      // NOTE: no need to check m==0 due to if statement construction
      if(j < m) add_nodes = d + 1;
      else      add_nodes = d;
      successfully_added += enclaves[i]->populate_initial(add_nodes, range, global_seed, &last);
   }
   usleep(10);

   size = data_layer_size(sentinel_node, 1);
   printf("Set size     : %d\n", size);
   printf("Level max    : %d\n", levelmax);

   // nullify index nodes to rebalance sl (deprecated)

   // Reset helper thread with appropriate sleep time
   for(int i = 0; i < num_numa_zones; ++i) {
      enclaves[i]->stop_helper();
      enclaves[i]->start_helper(1000000);
   }

   barrier_init(&barrier, nb_threads + 1);
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
   for (i = 0; i < nb_threads; i++) {
      data[i].first = last;
      data[i].range = range;
      data[i].update = update;
      data[i].alternate = alternate;
      data[i].effective = effective;
      data[i].seed = rand();
      data[i].stop = &stop;
      data[i].barrier = &barrier;
      enclaves[i]->start_application(data);
   }
   pthread_attr_destroy(&attr);

   // Catch some signals
   if (signal(SIGHUP, catcher) == SIG_ERR ||
      //signal(SIGINT, catcher) == SIG_ERR ||
      signal(SIGTERM, catcher) == SIG_ERR) {
      perror("signal");
      exit(1);
   }

   // Start threads
   barrier_cross(&barrier);

   printf("STARTING...\n");
   gettimeofday(&start, NULL);
   if (duration > 0) {
      nanosleep(&timeout, NULL);
   } else {
      sigemptyset(&block_set);
      sigsuspend(&block_set);
   }
   AO_store_full(&stop, 1);

   gettimeofday(&end, NULL);
   printf("STOPPING...\n");

   // Wait for thread completion
   for (i = 0; i < nb_threads; i++) {
      app_res* results;
      if (pthread_join(threads[i], (void**)&results) != 0) {
         fprintf(stderr, "Error waiting for thread completion\n");
         exit(1);
      }
      reads += results[i].contains;
      effreads += results[i].contains +
                 (results[i].add - results[i].added) +
                 (results[i].remove - results[i].removed);
      updates += (results[i].add + results[i].remove);
      adds += results[i].added;
      removes += results[i].removed;
      effupds += results[i].removed + results[i].added;
      size += results[i].added - results[i].removed;
      /*
      printf("Thread %d\n", i);
      printf("  #add        : %lu\n", results[i].add);
      printf("    #added    : %lu\n", results[i].added);
      printf("  #remove     : %lu\n", results[i].remove);
      printf("    #removed  : %lu\n", results[i].removed);
      printf("  #contains   : %lu\n", results[i].contains);
      printf("  #found      : %lu\n", results[i].found);
      */
   }
   duration = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);

   printf("Set size      : %d (expected: %d)\n", data_layer_size(sentinel_node,1), size);
   printf("Duration      : %d (ms)\n", duration);
   printf("#txs          : %lu (%f / s)\n", reads + updates, (reads + updates) * 1000.0 / duration);
   printf("#read txs     : ");
   if (effective) {
      printf("%lu (%f / s)\n", effreads, effreads * 1000.0 / duration);
      printf("  #contains   : %lu (%f / s)\n", reads, reads * 1000.0 / duration);
   } else { printf("%lu (%f / s)\n", reads, reads * 1000.0 / duration); }
   
   printf("#eff. upd rate: %f \n", 100.0 * effupds / (effupds + effreads));
   printf("#update txs   : ");
   if (effective) {
      printf("%lu (%f / s)\n", effupds, effupds * 1000.0 / duration);
      printf("  #adds: %lu(%f /s)\n", adds, adds * 1000.0 / duration);
      printf("  #rmvs: %lu(%f /s)\n", removes, removes * 1000.0 / duration);
      printf("  #upd trials : %lu (%f / s)\n", updates, updates * 1000.0 / duration);
   } else { printf("%lu (%f / s)\n", updates, updates * 1000.0 / duration); }

#ifdef ADDRESS_CHECKING
   int app_local = 0;
   int app_foreign = 0;
   int bkg_local = 0;
   int bkg_foreign = 0;
   for(int i = 0; i < num_numa_zones; ++i) {
      bkg_local += enclaves[i]->bg_local_accesses;
      bkg_foreign += enclaves[i]->bg_foreign_accesses;
      app_local += enclaves[i]->ap_local_accesses;
      app_foreign += enclaves[i]->ap_foreign_accesses;
   }
   printf("Application threads: %f%% local\n", (app_local * 100.0)/(app_local + app_foreign));
   printf(" #local accesses:   %d\n", app_local);
   printf(" #foreign accesses: %d\n", app_foreign);
   printf("Background threads: %f%% local\n", (bkg_local * 100.0)/(bkg_local + bkg_foreign));
   printf(" #local accesses:   %d\n", bkg_local);
   printf(" #foreign accesses: %d\n", bkg_foreign);
#endif

   printf("Cleaning up...\n");
   // Stop background threads
   for(int i = 0; i < num_numa_zones; ++i) {
      enclaves[i]->stop_helper();
   }
   return 0;
}

