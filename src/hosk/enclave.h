/*
 * Interface for search layer object
 *
 * Author: Henry Daly, 2017
 */
#ifndef ENCLAVE_H_
#define ENCLAVE_H_

//#include "queue.h"
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


class enclave {
private:
   inode_t*    sentinel;      // sentinel node of the index layer
   pthread_t   hlpth;         // helper pthread
   pthread_t   appth;         // application pthread
   op_t*       opbuffer;      // successful local operation array
   bool        index_busy;    // mutex for pthreads over index layer
   int         cpu_num;       // cpu on which enclave executes
   int         numa_zone;     // NUMA zone on which enclave executes
   int         non_del;       // # non deleted intermediate nodes
   int         tall_del;      // # deleted intermediate nodes w/ towers above
   int         buf_size;      // size of the circular op array
   int         app_idx;       // index of application thread in circular array
   int         hlp_idx;       // index of helper thread in circular array
public:
   bool  finished;
   bool  running;
   int   sleep_time;
//   bool  repopulate;
   
            enclave(int size, int cpu, int zone, inode_t* sent);
           ~enclave();
   void     start_helper(int);
   void     stop_helper(void);
   void     start_application(XXX); // we'll need a data structure to initialize with
   YYY      stop_application(void);
   inode_t* get_sentinel(void);
   inode_t* set_sentinel(inode_t*);
   int      get_cpu(void);
   int      get_numa_zone(void);
   bool     opbuffer_insert(sl_key_t key, node_t* node);
   op_t     opbuffer_remove(op_t passed);
   //void     reset_sentinel(void);

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
#endif
