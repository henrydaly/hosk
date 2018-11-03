/*
 * Interface for search layer object
 *
 * Author: Henry Daly, 2018
 */
#ifndef ENCLAVE_H_
#define ENCLAVE_H_
#include "skiplist.h"

// Uncomment to collect background stats - reduces performance
//#define BG_STATS
#ifdef BG_STATS
   typedef struct bg_stats bg_stats_t;
   struct bg_stats {
      int raises;
      int loops;
      int lowers;
      int delete_succeeds;
   };
#endif

/* op_t is the element which the enclave's circular array will contain.
   a node value of NULL implies the operation was a remove */
struct op_t {
   sl_key_t   key;
   node_t*    node;
   op_t():key(0), node(NULL){}
};

struct app_param {
   unsigned int   first;
   long           range;
   int            update;
   int            alternate;
   int            effective;
   unsigned int   seed;
   barrier_t*     barrier;
   VOLATILE AO_t* stop;
};

struct init_param {
   int   num;
   long  range;
   uint  seed;
   uint* last;
};

struct app_res {
   unsigned long add;
   unsigned long added;
   unsigned long remove;
   unsigned long removed;
   unsigned long contains;
   unsigned long found;
   unsigned long nb_aborts;
   unsigned long nb_aborts_locked_read;
   unsigned long nb_aborts_locked_write;
   unsigned long nb_aborts_validate_read;
   unsigned long nb_aborts_validate_write;
   unsigned long nb_aborts_validate_commit;
   unsigned long nb_aborts_invalid_memory;
   unsigned long nb_aborts_double_write;
   unsigned long max_retries;
   unsigned long failures_because_contention;
};

class enclave {
private:
   inode_t*    sentinel;      // sentinel node of the index layer
   pthread_t   hlpth;         // helper pthread
   pthread_t   appth;         // application pthread
   op_t*       opbuffer;      // successful local operation array
   int         cpu_num;       // cpu on which enclave executes
   int         numa_zone;     // NUMA zone on which enclave executes
   int         buf_size;      // size of the circular op array
   int         app_idx;       // index of application thread in circular array
   int         hlp_idx;       // index of helper thread in circular array

public:
   app_param*  aparams;        // parameters for the application thread execution
   init_param* iparams;        // parameters for population
   bool        finished;       // represents if helper thread is finished
   bool        running;        // represents if helper thread is running
   int         sleep_time;     // time for helper thread to sleep between loops
   int         num_populated;  // number of elements inserted during initial population
   int         non_del;        // # non deleted intermediate nodes
   int         tall_del;       // # deleted intermediate nodes w/ towers above
   uint        update_seed;   // seed for helper thread random generator
   int         update_freq;   // frequency of index layer updates

            enclave(int size, int cpu, int zone, inode_t* sent, int freq);
           ~enclave();
   void     start_helper(int);
   void     stop_helper(void);
   void     start_application(app_param* init);
   app_res* stop_application(void);
   inode_t* get_sentinel(void);
   inode_t* set_sentinel(inode_t*);
   int      get_cpu(void);
   int      get_numa_zone(void);
   bool     opbuffer_insert(sl_key_t key, node_t* node);
   op_t*    opbuffer_remove(op_t* passed);
   int      populate_initial(int num, long range, uint seed, uint* last);

#ifdef ADDRESS_CHECKING
   bool           index_ignore;
   volatile long  bg_local_accesses;
   volatile long  bg_foreign_accesses;
   volatile long  ap_local_accesses;
   volatile long  ap_foreign_accesses;
#endif
   #ifdef BG_STATS
   bg_stats_t shadow_stats;
   void bg_stats(void);
#endif
};

/* Public interface for application and helper thread functions */
void* initial_populate(void* args);
void* application_loop(void* args);
void* helper_loop(void* args);
void  node_remove(node_t* prev, node_t* node);
void barrier_init(barrier_t *b, int n);
void barrier_cross(barrier_t *b);
#endif
