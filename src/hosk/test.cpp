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
#include "hardware_layout.h"
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
#define DEFAULT_PARTITION              0
#define SOCKET_MAX                     numa_max_node() + 1
#define SOCKET_MIN                     1
#define XSTR(s)                        STR(s)
#define STR(s)                         #s
#define ATOMIC_CAS_MB(a, e, v)         (AO_compare_and_swap_full((VOLATILE AO_t *)(a), (AO_t)(e), (AO_t)(v)))
#define ATOMIC_FETCH_AND_INC_FULL(a)   (AO_fetch_and_add1_full((VOLATILE AO_t *)(a)))
#define VAL_MIN                        INT_MIN
#define VAL_MAX                        INT_MAX

struct tinit_args {
   int      enclave_num;
   core_t*  core;
   uint     sock_num;
   uint     index_allocator;
   uint     data_allocator;
};

int num_sockets = SOCKET_MAX;
VOLATILE AO_t stop;
unsigned int global_seed;
pthread_key_t rng_seed_key;
unsigned int levelmax;
enclave** enclaves;
tinit_args** zargs;
extern numa_allocator** allocators;
node_t* sentinel_node;
bool base_malloc = true;

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

void catcher(int sig) { printf("CAUGHT SIGNAL %d\n", sig); }

/* thread_init() - initializes the enclave object for a thread */
void* thread_init(void* args) {
   tinit_args* zia = (tinit_args*)args;
   int athread_id = zia->core->hwthread_id[APP_THD_ID];

   // Pin to CPU
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(athread_id, &cpuset);
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
   numa_set_preferred(zia->sock_num);
   sleep(1);

   numa_allocator* na = new numa_allocator(zia->data_allocator, zia->index_allocator);
   allocators[zia->enclave_num] = na;
   node_t*  dnode = node_new(0, NULL, NULL, sentinel_node, NULL, zia->enclave_num);
   dnode->level = 1;
   inode_t* inode = inode_new(NULL, NULL, dnode, zia->enclave_num);
   enclave* en = new enclave(zia->core, zia->sock_num, inode, zia->enclave_num);
   enclaves[zia->enclave_num] = en;
   return NULL;
}


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
   pthread_t bg_rmvl_thd;
   bg_args bg_rmvl_args;
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
   sigset_t block_set;
   struct sl_node *temp;
   int unbalanced = DEFAULT_UNBALANCED;
   int partition = DEFAULT_PARTITION;
   while(1) {
      i = 0;
      c = getopt_long(argc, argv, "hAf:d:i:t:r:S:u:U:z:p", long_options, &i);
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
                   "        Number of sockets to use (default = " XSTR(SOCKET_MAX) ")\n"
                   "  -p\n"
                   "        Partition the range of values over the enclaves\n"
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
            num_sockets = atoi(optarg);
            break;
         case 'p':
            partition = 1;
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
   assert(num_sockets >= SOCKET_MIN && num_sockets <= SOCKET_MAX);

   // Get hardware info
   hl_t* cur_hw = get_hardware_layout();

   int max_thread_num = cur_hw->max_cpu_num;
   if(nb_threads * 2 > max_thread_num) {
      printf("ERROR: application thread <= %d (max hw threads) / 2. Changing to %d.\n", max_thread_num, (max_thread_num / 2));
   }

   printf("Set type     : skip list\n");
   printf("Duration     : %d\n", duration);
   printf("Initial size : %u\n", initial);
   printf("Nb threads   : %d\n", nb_threads);
   printf("Value range  : %ld\n", range);
   printf("Seed         : %d\n", seed);
   printf("Update rate  : %d\n", update);
   printf("Alternate    : %d\n", alternate);
   printf("Effective    : %d\n", effective);
   printf("Type sizes   : int=%d/long=%d/ptr=%d/word=%d\n", (int)sizeof(int), (int)sizeof(long), (int)sizeof(void *), (int)sizeof(uintptr_t));
   printf("Sockets      : %d\n", num_sockets);

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
   levelmax = floor_log_2((unsigned int) initial / nb_threads);

   // sentinel node - used for initial population
   node_t* sn = (node_t*)malloc(sizeof(node_t));
   sn->key = 0;
   sn->val = NULL;
   sn->prev = sn->next = sn->local_next = NULL;
   sn->level = 0;
   sentinel_node = sn;
   // HOSK setup
   enclaves        =   (enclave**)malloc(nb_threads*sizeof(enclave*));
   pthread_t* thds =  (pthread_t*)malloc(nb_threads*sizeof(pthread_t));
   zargs           =(tinit_args**)malloc(nb_threads*sizeof(tinit_args*));
   allocators = (numa_allocator**)malloc(nb_threads*sizeof(numa_allocator*));
   uint num_expected_nodes = (unsigned)((initial / nb_threads) * (1.0 + (update/100.0)));
   uint dat_multiplier = 1000;
   uint idx_multiplier = 3;
   uint data_buf_size  = CACHE_LINE_SIZE * num_expected_nodes * dat_multiplier;
   uint index_buf_size = CACHE_LINE_SIZE * num_expected_nodes * idx_multiplier;
   int sock_id = 0;
   int core_id = 0;
   for(int i = 0; i < nb_threads; ++i) {
      tinit_args* zia      = (tinit_args*)malloc(sizeof(tinit_args));
      socket_t cur_sock    = cur_hw->sockets[sock_id];
      zia->data_allocator  = data_buf_size;
      zia->index_allocator = index_buf_size;
      zia->core            = &cur_sock.cores[core_id];
      zia->sock_num        = sock_id;
      zia->enclave_num     = i;
      zargs[i] = zia;
      pthread_create(&thds[i], NULL, thread_init, (void*)zia);
      sock_id++;
      if(sock_id == cur_hw->num_sockets || sock_id == num_sockets) {
         sock_id = 0;
         core_id++;
      }
   }
   for(int i = 0; i < nb_threads; ++i) {
      pthread_join(thds[i], NULL);
      free(zargs[i]);
   }
   free(thds);
   free(zargs);
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
   init_param** pop_params = (init_param**)malloc(sizeof(init_param*));
   int num_to_pop = 0;
   for(int j = 0; j < nb_threads; ++j) {
      pop_params[i] = (init_param*)malloc(sizeof(init_param));
      pop_params[i]->seed = seed;
      pop_params[i]->last = &last;
      // if size !divide across threads -> first m threads get + 1
      // NOTE: no need to check m==0 due to if statement construction
      if(j < m) num_to_pop = d + 1;
      else      num_to_pop = d;
      if(partition) {
         pop_params[i]->range  = range / nb_threads;
         pop_params[i]->offset = pop_params[i]->range * j;
      } else {
         pop_params[i]->range = range;
         pop_params[i]->offset = 0;
      }
      enclaves[j]->populate_begin(pop_params[i], num_to_pop);
   }
   for(int k = 0; k < nb_threads; ++k) {
      last = enclaves[k]->populate_end();
      enclaves[k]->stop_helper();
      free(pop_params[i]);
   }
   free(pop_params);
   base_malloc = false;
   reset_node_levels(sentinel_node);
   for(int k = 0; k < nb_threads; ++k) {
      enclaves[k]->reset_index_layer();
      enclaves[k]->start_helper(0);
   }
   free(pop_params);

   size = data_layer_size(sentinel_node, 1);
   printf("Set size     : %d\n", size);
   printf("Level max    : %d\n", levelmax);

   // Reset helper thread with appropriate sleep time
   for(int i = 0; i < nb_threads; ++i) {
      while(enclaves[i]->get_sentinel()->node->level < (floor_log_2(d) - 1)){}
      enclaves[i]->stop_helper();
      enclaves[i]->start_helper(0); //(100000);
      //printf("  Level of enclave %2d: %d\n", i, enclaves[i]->get_sentinel()->node->level);
   }

   barrier_init(&barrier, nb_threads + 1);
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
   for (i = 0; i < nb_threads; i++) {
      data[i].first = last;
      data[i].update = update;
      data[i].alternate = alternate;
      data[i].effective = effective;
      data[i].seed = rand();
      data[i].stop = &stop;
      data[i].barrier = &barrier;
      if(partition) {
         data[i].range  = range / nb_threads;
         data[i].offset = data[i].range * i;
      } else {
         data[i].range  = range;
         data[i].offset = 0;
      }
      enclaves[i]->start_application(&data[i]);
   }
   pthread_attr_destroy(&attr);

   // Catch some signals
   if (signal(SIGHUP, catcher) == SIG_ERR ||
      //signal(SIGINT, catcher) == SIG_ERR ||
      signal(SIGTERM, catcher) == SIG_ERR) {
      perror("signal");
      exit(1);
   }

   // Start the background removal thread
   if(update) {
      bg_rmvl_args.done = false;
      bg_rmvl_args.sentinel = sentinel_node;
      bg_rmvl_args.sockets = num_sockets;
      bg_rmvl_args.sleep = 15000;
      pthread_create(&bg_rmvl_thd, NULL, removal_loop, (void*)&bg_rmvl_args);
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

   // Stop threads
   AO_store_full(&stop, 1);
   gettimeofday(&end, NULL);
   printf("STOPPING...\n");
   bg_rmvl_args.done = true;

   // Wait for thread completion
   for (i = 0; i < nb_threads; i++) {
      app_res* results = enclaves[i]->stop_application();
      reads += results->contains;
      effreads += results->contains +
                 (results->add - results->added) +
                 (results->remove - results->removed);
      updates += (results->add + results->remove);
      adds += results->added;
      removes += results->removed;
      effupds += results->removed + results->added;
      size += results->added - results->removed;
      /*
      printf("Thread %d\n", i);
      printf("  #add        : %lu\n", results->add);
      printf("    #added    : %lu\n", results->added);
      printf("  #remove     : %lu\n", results->remove);
      printf("    #removed  : %lu\n", results->removed);
      printf("  #contains   : %lu\n", results->contains);
      printf("  #found      : %lu\n", results->found);
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
#ifdef COUNT_TRAVERSAL
   uint tavg_idx_trav = 0, tavg_dat_local_trav = 0, tavg_dat_trav = 0;
   for(int j = 0; j < nb_threads; ++j) {
      tavg_idx_trav       += enclaves[j]->trav_idx / enclaves[j]->total_ops;
      tavg_dat_trav       += enclaves[j]->trav_dat / enclaves[j]->total_ops;
      tavg_dat_local_trav += enclaves[j]->trav_dat_local / enclaves[j]->total_ops;
   }
   tavg_idx_trav       /= nb_threads;
   tavg_dat_trav       /= nb_threads;
   tavg_dat_local_trav /= nb_threads;
   printf("Average Index    Hops: %d\n", tavg_idx_trav);
   printf("Average Skiplink Hops: %d\n", tavg_dat_local_trav);
   printf("Average Data     Hops: %d\n", tavg_dat_trav);
#endif
#ifdef ADDRESS_CHECKING
   int app_local = 0;
   int app_foreign = 0;
   int bkg_local = 0;
   int bkg_foreign = 0;
   for(int i = 0; i < nb_threads; ++i) {
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
   if(update) {
      pthread_join(bg_rmvl_thd, NULL);
   }
   // Stop background threads
   for(int i = 0; i < nb_threads; ++i) {
      enclaves[i]->stop_helper();
   }
   for(int i = 0; i < nb_threads; ++i) {
      delete enclaves[i];
      delete allocators[i];
   }
   free_hardware_layout(cur_hw);
   free(sn);
   free(threads);
   free(data);
   free(allocators);
   free(enclaves);
   return 0;
}

