/*
 * nohotspot_ops.c: contains/insert/delete skip list operations
 *
 * Author: Ian Dick, 2013
 *
 *
 * NUMASK changes: use enclave object instead of set struct
 *    ++ on successful operation - set status field of node
 *    ++ address checking (if defined)
 *
 */

/*

Module Overview

This module provides the three basic skip list operations:
> insert(key, val)
> contains(key)
> delete(key)

These abstract operations are implemented using the algorithms
described in:
Crain, T., Gramoli, V., Raynal, M. (2013)
"No Hotspot Non-Blocking Skip List", to appear in
The 33rd IEEE International Conference on Distributed Computing
Systems (ICDCS).

This multi-thread skip list implementation is non-blocking,
and uses the Compare-and-swap hardware primitive to manage
concurrency and thread synchronisation. As a result,
the three public operations described in this module require
rather complicated implementations, in order to ensure that
the data structure can be used correctly.

All the abstract operations first use the private function
sl_do_operation() to retrieve two nodes from the skip list:
node - the node with greatest key k such that k is less than
       or equal to the search key
next - the node with the lowest key k such that k is greater than
       the search key
After node and next have been retrieved using sl_do_operation(),
the appropriate action is taken based on the abstract operation
being performed. For example, if we are conducting a delete and
node.key is equal to the search key, then we try to extract node.

Worthy to note is that worker threads in this skip list implementation
do not perform maintenance of the index levels: this is carried
out by a background thread. Worker threads do, however, perform
maintenance on the node level. During sl_do_operation(), if a worker
thread notices that a node it is traversing is logically deleted,
it will stop what it is doing and attempt to finish deleting the
node, and then resume it's previous course of action once this is
finished.

*/

#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <atomic_ops.h>
#include "application.h"
#include "skiplist.h"
#include "enclave.h"

/* private functions */
static int sl_finish_contains(sl_key_t key, node_t *node, val_t node_val);
static int sl_finish_delete(sl_key_t key, node_t *node, val_t node_val);
static int sl_finish_insert(sl_key_t key, val_t val, node_t* node, val_t node_val, node_t* next, node_t* pnode);

/**
 * sl_finish_contains() - contains skip list operation
 * @key      - the search key
 * @node     - the left node from sl_traverse_data()
 * @node_val - @node value
 *
 * Returns 1 if the search key is present and 0 otherwise.
 */
static int sl_finish_contains(sl_key_t key, node_t *node, val_t node_val) {
   int result = 0;
   assert(NULL != node);
   if ((key == node->key) && (NULL != node_val)) result = 1;
   return result;
}

/**
 * sl_finish_delete() - delete skip list operation
 * @key      - the search key
 * @node     - the left node from sl_traverse_data()
 * @node_val - @node value
 *
 * Returns 1 on success, 0 if the search key is not present,
 * and -1 if the key is present but the node is already
 * logically deleted, or if the CAS to logically delete fails.
 */
static int sl_finish_delete(sl_key_t key, node_t *node, val_t node_val) {
   int result = -1;
   assert(NULL != node);

   if (node->key != key) {
      result = 0; 
   } else {
      if (NULL != node_val) {
         /* loop until we or someone else deletes */
         while (1) {
            node_val = node->val;
            if (NULL == node_val || node == node_val) {
               result = 0;
               break;
            } else if (CAS(&node->val, node_val, NULL)) {
               result = 1;
               break;
            }
         }
      } else {
         /* Already logically deleted */
         result = 0;
      }
   }
   return result;
}

/**
 * sl_finish_insert() - insert skip list operation
 * @key      - the search key
 * @val      - the search value
 * @node     - the left node from sl_traverse_data()
 * @node_val - @node value
 * @next     - the right node from sl_traverse_data()
 * @pnode    - passed pointer set to successfully inserted node
 *
 * Returns:
 * > 1 if @key is present in the set and the corresponding node
 *   is logically deleted and the undeletion operation succeeds.
 * > 1 if @key is not present in the set and insertion operation
 *   succeeds.
 * > 0 if @key is present in the set and not null.
 * > -1 if @key is present in the set and value of corresponding
 *   node is not null and logical un-deletion fails due to concurrency.
 * > -1 if @key is not present in the set and insertion operation
 *   fails due to concurrency.
 */
static int sl_finish_insert(sl_key_t key, val_t val, node_t *node,
      val_t node_val, node_t *next, node_t* pnode) {
   int result = -1;
   node_t *newNode;
   if (node->key == key) {
      if (NULL == node_val) {
         if (CAS(&node->val, node_val, val)) {
            result = 1;
            pnode = node;
         }
      } else { result = 0; }
   } else {
      newNode = node_new(key, val, node, next);
      if (CAS(&node->next, next, newNode)) {
         assert (node->next != node);
         if (NULL != next) { next->prev = newNode; } /* safe */
         result = 1;
         pnode = newNode;
      } else {
         node_delete(newNode);
      }
   }
   return result;
}


/* - The public nohotspot_ops interface - */

/**
 * sl_traverse_index() - traverse index layer and return entry point to data layer
 * @obj - the enclave
 * @key - the search key
 */
node_t* sl_traverse_index(enclave* obj, sl_key_t key) {
   inode_t *item, *next_item;
   node_t* ret_node = NULL;
   item = obj->get_sentinel();
   int this_node = obj->get_numa_zone();
#ifdef ADDRESS_CHECKING
   zone_access_check(this_node, item, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
   while (1) {
      next_item = item->right;
#ifdef ADDRESS_CHECKING
      zone_access_check(this_node, next_item, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
      if (NULL == next_item || next_item->key > key) {
         next_item = item->down;
#ifdef ADDRESS_CHECKING
         zone_access_check(this_node, next_item, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
         if (NULL == next_item) {
            ret_node = item->intermed->node;
#ifdef ADDRESS_CHECKING
            zone_access_check(this_node, ret_node, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
            break;
         }
      } else if (next_item->key == key) {
         ret_node = item->intermed->node;
#ifdef ADDRESS_CHECKING
         zone_access_check(this_node, ret_node, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
         break;
      }
      item = next_item;
   }
   return ret_node;
}

/**
 * sl_traverse_data() - traverse data layer and finish assigned operation
 * NOTE: physical removal is attempted on logically deleted nodes
 * @obj    - the enclave
 * @node   - the entry point element on the data layer
 * @optype - the type of operation this is
 * @key    - the search key
 * @val    - the search value
 * @pnode  - pointer to the successfully updated node
 */
int sl_traverse_data(enclave* obj, node_t* node, sl_optype_t optype,
      sl_key_t key, val_t val, node_t* pnode) {
   node_t* next = NULL;
   val_t node_val = NULL, next_val = NULL;
   int result = 0;
   int this_node = obj->get_numa_zone();
   while (1) {
      while (node == (node_val = node->val)) {
         node = node->prev;
#ifdef ADDRESS_CHECKING
         zone_access_check(this_node, node, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
      }
      next = node->next;
#ifdef ADDRESS_CHECKING
      zone_access_check(this_node, next, &obj->ap_local_accesses, &obj->ap_foreign_accesses, false);
#endif
      if(NULL != next) {
         next_val = next->val;
         if((node_t*)next_val == next) {
            node_remove(node, next);
            continue;
         }
      }
      if (NULL == next || next->key > key) {
         if (CONTAINS == optype) {
            result = sl_finish_contains(key, node, node_val);
         } else if (DELETE == optype) {
            result = sl_finish_delete(key, node, node_val);
         } else if (INSERT == optype) {
            result = sl_finish_insert(key, val, node, node_val, next, pnode);
         }
         if (-1 != result) break;
         continue;
      }
      node = next;
   }
   return result;
}

/**
 * application_loop() - defines the execution flow of the application thread in each enclave
 * @args - the enclave object that owns the application thread
 */
void* application_loop(void* args) {
   enclave*    obj      = (enclave*)args;
   inode_t*    sentinel = obj->get_sentinel();
   int         numa_zone= obj->get_numa_zone();
   app_params* params   = obj->params;
   app_res*    lresults = new app_res();
   VOLATILE AO_t stop   = params->stop;
   int unext, last = -1;
   unsigned int key = 0;
   sl_optype_t otype = CONTAINS;
   val_t val;

   // Pin to CPU
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(obj->get_cpu(), &cpuset);
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

   /* Is the first op an update? */
   unext = (rand_range_re(&params->seed, 100) - 1 < params->update);

   while(AO_load_full(&stop) == 0) {

      if(unext) { // update
         if (last < 0) { // add
            key = rand_range_re(&params->seed, params->range);
            otype = INSERT;
         } else { // remove
            if (params->alternate) { // alternate mode (default)
               key = last;
               otype = DELETE;
            } else {
               key = rand_range_re(&params->seed, params->range);
            }
         }
      } else { // read
         otype = CONTAINS;
         if(params->alternate) {
            if(params->update == 0) {
            	if(last < 0) {
            		key = params->first;
            		last = key;
            	} else {
            	   key = rand_range_re(&params->seed, params->range);
                  last = -1;
            	}
            } else { // update != 0
               if(last < 0) {
                  key = rand_range_re(&params->seed, params->range);
               } else {
                  key = last;
               }
            }
         } else {
            key = rand_range_re(&params->seed, params->range);
         }

      }
      val = (val_t)((long)key);
      if(!obj->index_busy) {
         if(CAS(&obj->index_busy, false, true)) {
            // Application thread has exclusive access to the index layer
            node_t* node = sl_traverse_index(obj, key);
            obj->index_busy = false;
            node_t* pnode = NULL;
            int result = sl_traverse_data(obj, node, otype, key, val, pnode);
            last = update_results(otype, lresults, result, key, last, params->alternate);
            if(result == 1 && otype != CONTAINS) obj->opbuffer_insert(key, pnode);
         }
      }
      unext = get_unext(params, lresults);
   }
   return lresults;
}
