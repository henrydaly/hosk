/*
 * enclave.cpp: Definition of enclave functionality
 *
 * Author: Henry Daly, 2018
 */

/**
 * Module Overview:
 *
 * The enclave class provides an abstraction for an application
 * and helper thread running on the same core.
 */

#include <pthread.h>
#include "enclave.h"
#include "hardware_layout.h"
#include "skiplist.h"
#include "stdio.h"
/* Constructor */
enclave::enclave(core_t* c, int sock, inode_t* s, int e_num)
 :core(c), socket_num(sock), sentinel(s), enclave_num(e_num)
{
   update_seed = rand();
   aparams = NULL;
   iparams = NULL;
   app_idx = hlp_idx = tall_del = non_del = 0;
   finished = running = reset_index = false;
   hlpth = appth = num_populate = sleep_time = 0;
#ifdef COUNT_TRAVERSAL
   trav_idx = trav_dat = trav_dat_local = total_ops = 0;
#endif
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
void enclave::start_application(app_param* init) {
   aparams = init;
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

/**
 * get_thread_id() - return the cpu on which the enclave executes
 * @idx - either 0 (application thread) or 1 (helper thread)
 */
int enclave::get_thread_id(int idx) {
   return core->hwthread_id[idx];
}

/* get_enclave_num() - return the enclave id number */
int enclave::get_enclave_num(void) {
   return enclave_num;
}

/* get_socket_num() - return the enclave's socket id */
int enclave::get_socket_num(void) {
   return socket_num;
}

/* populate_begin() - populates num elements from local enclave */
void enclave::populate_begin(init_param* params, int num_to_pop) {
   iparams = params;
   num_populate = num_to_pop;
   pthread_create(&appth, NULL, initial_populate, (void*)this);
}

/* populate_end() - finishes population */
uint enclave::populate_end(void) {
   pthread_join(appth, NULL);
   return *(iparams->last);
}

/* reset_index() - resets index layers */
void enclave::reset_index_layer(void) {
   reset_index = true;
}

#ifdef BG_STATS
/* bg_stats() - print background statistics */
void enclave::bg_stats(void) {
   printf("Loops = %d\n", shadow_stats.loops);
   printf("Raises = %d\n", shadow_stats.raises);
   printf("Lowers = %d\n", shadow_stats.lowers);
   printf("Delete Succeeds = %d\n", shadow_stats.delete_succeeds);
}
#endif
