/*
 * enclave.cpp: search layer definition
 *
 * Author: Henry Daly, 2018
 */


/**
 * Module Overview:
 *
 * The enclave class provides an abstraction for the index and intermediate
 * layer of a NUMA zone. One object is created per NUMA zone.
 *
 */

#include <pthread.h>
#include "enclave.h"
#include "skiplist.h"
#include "stdio.h"

/* Constructor */
enclave::enclave(int size, int cpu, int zone, inode_t* s)
 :cpu_num(cpu), numa_zone(zone), sentinel(s)
{
   buf_size = size;
   opbuffer = new op_t[buf_size];
   for(int i = 0; i < buf_size; i++) {
      opbuffer[i] = op_t();
   }
   params=NULL;
   app_idx = hlp_idx = tall_del = non_del = 0;
   index_busy = finished = running = false;
   hlpth = appth = sleep_time = 0;
//   srand(time(NULL));
#ifdef BG_STATS
   shadow_stats.loops = 0;
   shadow_stats.raises = 0;
   shadow_stats.lowers = 0;
   shadow_stats.delete_succeeds = 0;
#endif
#ifdef ADDRESS_CHECKING
   bg_local_accesses = 0;
   bg_foreign_accesses = 0;
   ap_local_accesses = 0;
   ap_foreign_accesses = 0;
   index_ignore = true;
#endif
}

/* Destructor */
enclave::~enclave() {
   if(!finished) {
      finished = true;
      stop_helper();
      stop_application();
   }
}

/* start_helper() - starts helper thread */
void enclave::start_helper(int ssleep_time) {
   if(!running) {
      sleep_time = ssleep_time;
      running = true;
      finished = false;
      pthread_create(&hlpth, NULL, helper_loop, (void*)this);
   }
}

/* stop_helper() - stops helper thread */
void enclave::stop_helper(void) {
   if(running) {
      finished = true;
      pthread_join(hlpth, NULL);
      running = false;
   }
}

/* start_application() - starts application thread */
void enclave::start_application(app_params* init) {
   params = init;
   pthread_create(&appth, NULL, application_loop, (void*)this);
}

/* stop_application() - stops application thread*/
app_res* enclave::stop_application(void) {
   app_res* results = NULL;
   pthread_join(appth, (void**)&results);
   return results;
}

/* get_sentinel() - return sentinel index node of search layer */
inode_t* enclave::get_sentinel(void) {
   return sentinel;
}

/* set_sentinel() - update and return new sentinel node */
inode_t* enclave::set_sentinel(inode_t* new_sent) {
   return (sentinel = new_sent);
}

/* get_cpu() - return the cpu on which the enclave executes */
int enclave::get_cpu(void) {
   return cpu_num;
}

/* get_numa_zone() - return the NUMA zone on which the enclave executes */
int enclave::get_numa_zone(void) {
   return numa_zone;
}

/**
 * opbuffer_insert() - attempt to add element to the operation array
 *  NOTE: return false on failure
 * @key  - the key of the updated node
 * @node - the pointer to the updated node (NULL if operation was a remove)
 */
bool enclave::opbuffer_insert(sl_key_t key, node_t* node) {
   if((app_idx + 1) % buf_size == hlp_idx) return false;
   op_t cur_op = opbuffer[app_idx];
   cur_op.key = key;
   cur_op.node = node;
   app_idx = (app_idx + 1) % buf_size;
   return true;
}

/**
 * opbuffer_remove() - attempt to consume element in operation array
 *  the array element is copied by value into the passed element
 *  NOTE: return NULL on failure
 * @x - the element which will hold the copied array values
 */
op_t* enclave::opbuffer_remove(op_t* passed) {
   if((hlp_idx + 1) % buf_size == app_idx || hlp_idx == app_idx) return NULL;
   op_t cur_element = opbuffer[hlp_idx];
   passed->key  = cur_element.key;
   passed->node = cur_element.node;
   hlp_idx = (hlp_idx + 1) % buf_size;
   return passed;
}

/* reset_sentinel() - sets flag to later reset sentinel to fix towers */
//void enclave::reset_sentinel(void) {
//   repopulate = true;
//}

#ifdef BG_STATS
/**
 * bg_stats() - print background statistics
 */
void enclave::bg_stats(void)
{
   printf("Loops = %d\n", shadow_stats.loops);
   printf("Raises = %d\n", shadow_stats.raises);
   printf("Lowers = %d\n", shadow_stats.lowers);
   printf("Delete Succeeds = %d\n", shadow_stats.delete_succeeds);
}
#endif
