/*
 * Interface for enclave object
 *
 * Author: Henry Daly, 2018
 */
#ifndef ENCLAVE_H_
#define ENCLAVE_H_
#include "skiplist.h"
#define CAPACITY  2500

// Uncomment to collect stats on thread-local index and data layer traversal
//#define COUNT_TRAVERSAL

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

/* app_param defines the information passed to an application thread */
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

/* init_param defines the information passed to a enclave thread during
   initial population */
struct init_param {
   int   num;
   long  range;
   uint  seed;
   uint* last;
};

/* app_res defines the information returned to the main thread at the end
   of an application thread's execution */
struct app_res {
   unsigned long add;
   unsigned long added;
   unsigned long remove;
   unsigned long removed;
   unsigned long contains;
   unsigned long found;
};

class enclave {
private:
   inode_t*    sentinel;      // sentinel node of the index layer
   pthread_t   hlpth;         // helper pthread
   pthread_t   appth;         // application pthread
   op_t        opbuffer[CAPACITY];      // successful local operation array
   int         cpu_num;       // cpu on which enclave executes
   int         numa_zone;     // NUMA zone on which enclave executes
   int         buf_size;      // size of the circular op array
   int         app_idx;       // index of application thread in circular array
   int         hlp_idx;       // index of helper thread in circular array
   bool        running;       // represents if helper thread is running

public:
   app_param*  aparams;       // parameters for the application thread execution
   init_param* iparams;       // parameters for population
   int         non_del;       // # non deleted intermediate nodes
   int         tall_del;      // # deleted intermediate nodes w/ towers above
   uint        update_seed;   // seed for helper thread random generator
   int         update_freq;   // frequency of index layer updates
   int         num_populated; // number of elements inserted during initial population
   int         sleep_time;    // time for helper thread to sleep between loops
   bool        finished;      // represents if helper thread is finished

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
   op_t*    opbuffer_remove(op_t** passed);
   int      populate_initial(init_param* params);

#ifdef COUNT_TRAVERSAL
   uint trav_idx;
   uint trav_dat;
   uint total_ops;
#endif

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
