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
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <numa.h>
#include <atomic_ops.h>
#include "common.h"
#include "skiplist.h"
#include "enclave.h"
#include "allocator.h"

#define DEFAULT_DURATION               10000
#define DEFAULT_INITIAL                1024
#define DEFAULT_NB_THREADS             1
#define DEFAULT_RANGE                  0x7FFFFFFF
#define DEFAULT_SEED                   0
#define DEFAULT_UPDATE                 20
#define DEFAULT_ELASTICITY             4
#define DEFAULT_ALTERNATE              0
#define DEFAULT_EFFECTIVE              1
#define DEFAULT_UNBALANCED             0
#define MAX_NUMA_ZONES                 numa_max_node() + 1
#define MIN_NUMA_ZONES                 1

#define XSTR(s)                        STR(s)
#define STR(s)                         #s

#define ATOMIC_CAS_MB(a, e, v)         (AO_compare_and_swap_full((VOLATILE AO_t *)(a), (AO_t)(e), (AO_t)(v)))
#define ATOMIC_FETCH_AND_INC_FULL(a)   (AO_fetch_and_add1_full((VOLATILE AO_t *)(a)))

#define TRANSACTIONAL                  d->unit_tx

#define VAL_MIN                        INT_MIN
#define VAL_MAX                        INT_MAX

int num_numa_zones = MAX_NUMA_ZONES;

inline long rand_range(long r); /* declared in test.c */

int floor_log_2(unsigned int n) {
   int pos = 0;
   if (n >= 1<<16) { n >>= 16; pos += 16; }
   if (n >= 1<< 8) { n >>=  8; pos +=  8; }
   if (n >= 1<< 4) { n >>=  4; pos +=  4; }
   if (n >= 1<< 2) { n >>=  2; pos +=  2; }
   if (n >= 1<< 1) {           pos +=  1; }
   return ((n == 0) ? (-1) : pos);
}

/* NUMASK additions */
bool initial_populate;
enclave** enclaves;
extern numa_allocator** allocators;
struct zone_init_args {
   int      cpu_num;
   node_t*  node_sentinel;
   unsigned allocator_size;
};

/* zone_init() - initializes the queue and search layer object for a NUMA zone   */
void* zone_init(void* args) {
   int buffer_size = 1000; // TODO: buffer size
   zone_init_args* zia = (zone_init_args*)args;
   int numa_zone = zia->cpu_num % num_numa_zones;
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(zia->cpu_num, &cpuset);
   sleep(1);
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
   numa_set_preferred(numa_zone);
   if(allocators[numa_zone] == NULL) {
      numa_allocator* na = new numa_allocator(zia->allocator_size);
      allocators[numa_zone] = na;
   }
   mnode_t* mnode = mnode_new(NULL, zia->node_sentinel, 1, numa_zone);
   inode_t* inode = inode_new(NULL, NULL, mnode, numa_zone);
   enclave* en = new enclave(buffer_size, zia->cpu_num, numa_zone, inode);
   enclaves[zia->cpu_num] = en;
   return NULL;
}

void catcher(int sig)
{
   printf("CAUGHT SIGNAL %d\n", sig);
}

int main(int argc, char **argv)
{
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
   unsigned int last = 0;
   unsigned int val = 0;
   unsigned long reads, effreads, updates, effupds, aborts, aborts_locked_read, aborts_locked_write,
   aborts_validate_read, aborts_validate_write, aborts_validate_commit,
   aborts_invalid_memory, aborts_double_write, max_retries, failures_because_contention;
//   thread_data_t *data;
   pthread_t *threads;
   pthread_attr_t attr;
   barrier_t barrier;
   struct timeval start, end;
   struct timespec timeout;
   int duration = DEFAULT_DURATION;
   int initial = DEFAULT_INITIAL;
   int nb_threads = DEFAULT_NB_THREADS;
   long range = DEFAULT_RANGE;
   int seed = DEFAULT_SEED;
   int update = DEFAULT_UPDATE;
   int unit_tx = DEFAULT_ELASTICITY;
   int alternate = DEFAULT_ALTERNATE;
   int effective = DEFAULT_EFFECTIVE;
   sigset_t block_set;
   struct sl_node *temp;
   int unbalanced = DEFAULT_UNBALANCED;
   while(1) {
      i = 0;
      c = getopt_long(argc, argv, "hAf:d:i:t:r:S:u:x:U:z:P:", long_options, &i);

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
                   "  -x, --elasticity (default=4)\n"
                   "        Use elastic transactions\n"
                   "        0 = non-protected,\n"
                   "        1 = normal transaction,\n"
                   "        2 = read elastic-tx,\n"
                   "        3 = read/add elastic-tx,\n"
                   "        4 = read/add/rem elastic-tx,\n"
                   "        5 = fraser lock-free\n"
                   "  -z <int>\n"
                   "        Number of NUMA zones to use (default = " XSTR(MAX_NUMA_ZONES) ")\n" );
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
         case 'x':
            unit_tx = atoi(optarg);
            break;
         case 'U':
            unbalanced = atoi(optarg);
            break;
         case 'z':
            num_numa_zones = atoi(optarg);
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

   if(numa_available() == -1) {
      printf("Error: NUMA unavailable on this system.\n");
      exit(0);
   }

   printf("Set type     : skip list\n");
   printf("Duration     : %d\n", duration);
   printf("Initial size : %u\n", initial);
   printf("Nb threads   : %d\n", nb_threads);
   printf("Value range  : %ld\n", range);
   printf("Seed         : %d\n", seed);
   printf("Update rate  : %d\n", update);
   printf("Elasticity   : %d\n", unit_tx);
   printf("Alternate    : %d\n", alternate);
   printf("Efffective   : %d\n", effective);
   printf("Type sizes   : int=%d/long=%d/ptr=%d/word=%d\n", (int)sizeof(int), (int)sizeof(long), (int)sizeof(void *), (int)sizeof(uintptr_t));
   printf("NUMA Zones   : %d\n", num_numa_zones);

   timeout.tv_sec = duration / 1000;
   timeout.tv_nsec = (duration % 1000) * 1000000;

   if ((data = (thread_data_t *)malloc(nb_threads * sizeof(thread_data_t))) == NULL) {
      perror("malloc");
      exit(1);
   }
   if ((threads = (pthread_t *)malloc(nb_threads * sizeof(pthread_t))) == NULL) {
      perror("malloc");
      exit(1);
   }

   if (seed == 0) { srand((int)time(0)); }
   else           { srand(seed); }

   levelmax = floor_log_2((unsigned int) initial);

   // create sentinel node on NUMA zone 0
   node_t* sentinel_node = node_new(0, NULL, NULL, NULL);
   // create search layers
   enclaves = (enclave**)malloc(nb_threads*sizeof(enclave*));
   pthread_t* thds = (pthread_t*)malloc(num_numa_zones*sizeof(pthread_t));
   allocators = (numa_allocator**)malloc(num_numa_zones*sizeof(numa_allocator*));
   unsigned num_expected_nodes = (unsigned)(16 * initial * (1.0 + (update/100.0)));
   unsigned buffer_size = CACHE_LINE_SIZE * num_expected_nodes;

   zone_init_args* zia = (zone_init_args*)malloc(sizeof(zone_init_args));
   zia->node_sentinel = sentinel_node;
   zia->allocator_size = buffer_size;
   for(int i = 0; i < nb_threads; ++i) {
      zia->cpu_num = i;
      pthread_create(&thds[i], NULL, zone_init, (void*)zia);
   }

   for(int i = 0; i < num_numa_zones; ++i) {
      void *retval;
      pthread_join(thds[i], &retval);
   }
   stop = 0;
   global_seed = rand();
#ifdef TLS
   rng_seed = &global_seed;
#else /* ! TLS */
   if (pthread_key_create(&rng_seed_key, NULL) != 0) {
      fprintf(stderr, "Error creating thread local\n");
      exit(1);
   }
   pthread_setspecific(rng_seed_key, &global_seed);
#endif /* ! TLS */

   // Init STM
   printf("Initializing STM\n");

   TM_STARTUP();
   // Populate set
   printf("Adding %d entries to set\n", initial);
   i = 0;
   initial_populate = true;


   for(int i = 0; i < num_numa_zones; ++i) {
      search_layers[i]->start_helper(0);
   }

   int cur_zone = 0;
   numa_run_on_node(cur_zone);
   usleep(10);
   while (i < initial) {
      if (unbalanced) { val = rand_range_re(&global_seed, initial); } 
      else {            val = rand_range_re(&global_seed, range); }
      if (sl_add_old(search_layers[cur_zone], val, 0)) {
         last = val;
         i++;
         if(i %(initial / 4) == 0 && cur_zone != 3) {
            numa_run_on_node(++cur_zone);
         }
      }
   }

   size = data_layer_size(sentinel_node, 1);
   printf("Set size     : %d\n", size);
   printf("Level max    : %d\n", levelmax);
   initial_populate = false;

   // nullify all the index nodes we created so
   // we can start again and rebalance the skip list
   // wait until the list is balanced
   for(int i = 0; i < num_numa_zones; ++i) {
      search_layers[i]->stop_helper();
      search_layers[i]->reset_sentinel();
      search_layers[i]->start_helper(0);
   }
   for(int i = 0; i < num_numa_zones; ++i) {
      while(search_layers[i]->get_sentinel()->intermed->level < floor_log_2(initial)){}
      search_layers[i]->stop_helper();
      search_layers[i]->start_helper(1000000);
      printf("Number of levels in zone %d is %d\n", i, search_layers[i]->get_sentinel()->intermed->level);
   }

   barrier_init(&barrier, nb_threads + 1);
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
   int sl_index = 0;
   for (i = 0; i < nb_threads; i++) {
      data[i].first = last;
      data[i].range = range;
      data[i].update = update;
      data[i].unit_tx = unit_tx;
      data[i].alternate = alternate;
      data[i].effective = effective;
      data[i].nb_add = 0;
      data[i].nb_added = 0;
      data[i].nb_remove = 0;
      data[i].nb_removed = 0;
      data[i].nb_contains = 0;
      data[i].nb_found = 0;
      data[i].nb_aborts = 0;
      data[i].nb_aborts_locked_read = 0;
      data[i].nb_aborts_locked_write = 0;
      data[i].nb_aborts_validate_read = 0;
      data[i].nb_aborts_validate_write = 0;
      data[i].nb_aborts_validate_commit = 0;
      data[i].nb_aborts_invalid_memory = 0;
      data[i].nb_aborts_double_write = 0;
      data[i].max_retries = 0;
      data[i].seed = rand();
      data[i].sl = search_layers[sl_index++];
      data[i].barrier = &barrier;
      data[i].failures_because_contention = 0;
      if (pthread_create(&threads[i], &attr, test, (void *)(&data[i])) != 0) {
         fprintf(stderr, "Error creating thread\n");
         exit(1);
      }
      if(sl_index == num_numa_zones){ sl_index = 0; }
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
#ifdef ICC
   stop = 1;
#else
   AO_store_full(&stop, 1);
#endif /* ICC */

   gettimeofday(&end, NULL);
   printf("STOPPING...\n");

   // Wait for thread completion
   for (i = 0; i < nb_threads; i++) {
      if (pthread_join(threads[i], NULL) != 0) {
         fprintf(stderr, "Error waiting for thread completion\n");
         exit(1);
      }
   }
   duration = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);
   aborts = 0;
   aborts_locked_read = 0;
   aborts_locked_write = 0;
   aborts_validate_read = 0;
   aborts_validate_write = 0;
   aborts_validate_commit = 0;
   aborts_invalid_memory = 0;
   aborts_double_write = 0;
   failures_because_contention = 0;
   reads = 0;
   effreads = 0;
   updates = 0;
   effupds = 0;
   max_retries = 0;
   unsigned long adds = 0, removes = 0;
   for (i = 0; i < nb_threads; i++) {
   /*
      printf("Thread %d\n", i);
      printf("  #add        : %lu\n", data[i].nb_add);
      printf("    #added    : %lu\n", data[i].nb_added);
      printf("  #remove     : %lu\n", data[i].nb_remove);
      printf("    #removed  : %lu\n", data[i].nb_removed);
      printf("  #contains   : %lu\n", data[i].nb_contains);
      printf("  #found      : %lu\n", data[i].nb_found);
      printf("  #aborts     : %lu\n", data[i].nb_aborts);
      printf("    #lock-r   : %lu\n", data[i].nb_aborts_locked_read);
      printf("    #lock-w   : %lu\n", data[i].nb_aborts_locked_write);
      printf("    #val-r    : %lu\n", data[i].nb_aborts_validate_read);
      printf("    #val-w    : %lu\n", data[i].nb_aborts_validate_write);
      printf("    #val-c    : %lu\n", data[i].nb_aborts_validate_commit);
      printf("    #inv-mem  : %lu\n", data[i].nb_aborts_invalid_memory);
      printf("    #dup-w    : %lu\n", data[i].nb_aborts_double_write);
      printf("    #failures : %lu\n", data[i].failures_because_contention);
      printf("  Max retries : %lu\n", data[i].max_retries);
   */
   aborts += data[i].nb_aborts;
   aborts_locked_read += data[i].nb_aborts_locked_read;
   aborts_locked_write += data[i].nb_aborts_locked_write;
   aborts_validate_read += data[i].nb_aborts_validate_read;
   aborts_validate_write += data[i].nb_aborts_validate_write;
   aborts_validate_commit += data[i].nb_aborts_validate_commit;
   aborts_invalid_memory += data[i].nb_aborts_invalid_memory;
   aborts_double_write += data[i].nb_aborts_double_write;
   failures_because_contention += data[i].failures_because_contention;
   reads += data[i].nb_contains;
   effreads += data[i].nb_contains +
              (data[i].nb_add - data[i].nb_added) +
              (data[i].nb_remove - data[i].nb_removed);
   updates += (data[i].nb_add + data[i].nb_remove);
   adds += data[i].nb_added;
   removes += data[i].nb_removed;
   effupds += data[i].nb_removed + data[i].nb_added;
   size += data[i].nb_added - data[i].nb_removed;
   if (max_retries < data[i].max_retries)
      max_retries = data[i].max_retries;
   }

   printf("Set size      : %d (expected: %d)\n", data_layer_size(sentinel_node,1), size);
// printf("Size (w/ del) : %d\n", data_layer_size(sentinel_node, 0));
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

   printf("#aborts       : %lu (%f / s)\n", aborts, aborts * 1000.0 / duration);
   printf("  #lock-r     : %lu (%f / s)\n", aborts_locked_read, aborts_locked_read * 1000.0 / duration);
   printf("  #lock-w     : %lu (%f / s)\n", aborts_locked_write, aborts_locked_write * 1000.0 / duration);
   printf("  #val-r      : %lu (%f / s)\n", aborts_validate_read, aborts_validate_read * 1000.0 / duration);
   printf("  #val-w      : %lu (%f / s)\n", aborts_validate_write, aborts_validate_write * 1000.0 / duration);
   printf("  #val-c      : %lu (%f / s)\n", aborts_validate_commit, aborts_validate_commit * 1000.0 / duration);
   printf("  #inv-mem    : %lu (%f / s)\n", aborts_invalid_memory, aborts_invalid_memory * 1000.0 / duration);
   printf("  #dup-w      : %lu (%f / s)\n", aborts_double_write, aborts_double_write * 1000.0 / duration);
   printf("  #failures   : %lu\n",  failures_because_contention);
   printf("Max retries   : %lu\n", max_retries);

#ifdef ADDRESS_CHECKING
   int app_local = 0;
   int app_foreign = 0;
   int bkg_local = 0;
   int bkg_foreign = 0;
   for(int i = 0; i < num_numa_zones; ++i) {
      bkg_local += search_layers[i]->bg_local_accesses;
      bkg_foreign += search_layers[i]->bg_foreign_accesses;
      app_local += search_layers[i]->ap_local_accesses;
      app_foreign += search_layers[i]->ap_foreign_accesses;
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
   test_complete = true;
   for(int i = 0; i < num_numa_zones; ++i) {
      search_layers[i]->stop_helper();
   }

   // Cleanup STM
   TM_SHUTDOWN();
#ifndef TLS
   //pthread_key_delete(rng_seed_key);
#endif /* ! TLS */
   return 0;
}

