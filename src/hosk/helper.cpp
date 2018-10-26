/*
 * helper.cpp: helper thread index and intermediate layer maintenance
 *
 * Author: Henry Daly, 2018
 *
 * Built off NUMASK background.cpp (2017)
 */


/**
 * Module Overview:
 *
 * This provides two algorithms: one for managing the data layer (physical removals
 * and propagating updates to the NUMA zone queues) and one for managing the index
 * layers (index and intermediate layers) on each NUMA zone. One thread is spawned to
 * manage the data layer, and one thread per NUMA zone is spawned to manage that zone's
 * index layer. Based on No Hotspot's background.c file, this file splits bookkeeping
 * from the single helper thread into 1 + NUM_NUMA_ZONES threads to reduce NUMA
 * interconnect traffic.
 */

#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <numaif.h>
#include <atomic_ops.h>
#include <numa.h>
#include "common.h"
#include "helper.h"
#include "skiplist.h"
#include "enclave.h"
/**
 * reset_indermediate_levels() - iterates through intermediate level and sets their level to 0
 * @obj - search layer object for reference
 *
 * Note: this is only used after population for making balanced index towers
 */
void reset_intermediate_levels(enclave* obj) {
   mnode_t* node = obj->get_sentinel()->intermed;
   node->level = node->node->level = 1;
   mnode_t* next = node->next;
   while(next != NULL) {
      next->level = next->node->level = 0;
      next = next->next;
   }
}

/**
 * bg_mremove - starts the physical removal of @mnode
 * @prev  - the node before the one to remove
 * @mnode - the node to finish removing
 * @zone  - NUMA zone
 *
 * Note: since this operates on the intermediate layer alone,
 * no synchronization techniques are needed
 *
 * returns 1 if deleted, 0 if not
 */
int bg_mremove(mnode_t* prev, mnode_t* mnode, int zone) {
   int result = 0;
   assert(prev);
   assert(mnode);
   if(mnode->level == 0 && mnode->marked) {
      prev->next = mnode->next;
      mnode_delete(mnode, zone);
      result = 1;
   }
   return result;
}

/**
 * bg_trav_mnodes - traverse intermediate nodes and remove if possible
 * @obj - the enclave object for reference
 */
static void bg_trav_mnodes(enclave* obj) {
   mnode_t* prev = obj->get_sentinel()->intermed;
   mnode_t* node = prev->next;
#ifdef ADDRESS_CHECKING
   zone_access_check(zone, prev, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
   zone_access_check(zone, node, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif

   while (NULL != node) {
      if(bg_mremove(prev, node, obj->get_numa_zone())) {
         node = prev->next;
      } else {
         if(!node->marked)          { ++obj->non_del; }
         else if (node->level >= 1) { ++obj->tall_del; }
         prev = node;
         node = node->next;
      }
#ifdef ADDRESS_CHECKING
      zone_access_check(zone, node, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
   }
}

/**
 * update_intermediate_layer() - updates intermediate layer from local op array
 * @obj - enclave object for reference
 * @job - operation to publish to the intermediate layer
 */
void update_intermediate_layer(enclave* obj, op_t* job) {
   int      numa_zone= obj->get_numa_zone();
   sl_key_t test_key = job->key;
   inode_t* item     = obj->get_sentinel();
#ifdef ADDRESS_CHECKING
   zone_access_check(numa_zone, item, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
   inode_t* next_item = NULL;
   mnode_t *mnode, *next;

   // index layer traversal
   while(1) {
      next_item = item->right;
#ifdef ADDRESS_CHECKING
      zone_access_check(numa_zone, next_item, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
      if (NULL == next_item || next_item->key > test_key) {
         next_item = item->down;
#ifdef ADDRESS_CHECKING
         zone_access_check(numa_zone, next_item, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
         if(NULL == next_item) {
            mnode = item->intermed;
#ifdef ADDRESS_CHECKING
            zone_access_check(numa_zone, mnode, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
            break;
         }
      } else if (next_item->key == test_key) {
         mnode = item->intermed;
#ifdef ADDRESS_CHECKING
         zone_access_check(numa_zone, mnode, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
         break;
      }
      item = next_item;
   }

   // intermediate layer traversal and actual update
   while(1) {
      next = mnode->next;
#ifdef ADDRESS_CHECKING
      zone_access_check(numa_zone, next, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
      if(!next || next->key > test_key) {
         // if node pointer is not NULL, we know it's an insert
         if(job->node != NULL) {
            if(mnode->key == test_key) {
               if(mnode->marked) { mnode->marked = false; }
            } else {
               mnode->next = mnode_new(next, job->node, 0, numa_zone);
            }
         } else {
            if(mnode->key == test_key) { mnode->marked = true; }
         }
         break;
      }
      mnode = next;
   }
}

/**
 * bg_raise_mlevel - raise intermediate nodes into index levels
 * @mnode - starting intermediate node
 * @inode - starting index node at bottom layer
 * @zone  - local NUMA zone
 */
static int bg_raise_mlevel(mnode_t* mnode, inode_t* inode, int zone) {
   int raised = 0;
   mnode_t *prev, *node, *next;
   inode_t *inew, *above, *above_prev;
   above = above_prev = inode;
   assert(NULL != inode);

   prev = mnode;
   node = mnode->next;
   if (NULL == node) return 0;

   next = node->next;
   while (NULL != next) {
      /* don't raise deleted nodes */
      if (!mnode->marked) {
         if (((prev->level == 0) && (node->level == 0)) && (next->level == 0)) {
            raised = 1;

            /* get the correct index above and behind */
            while (above && above->intermed->key < node->key) {
               above = above->right;
               if (above != inode->right) { above_prev = above_prev->right; }
            }

            /* add a new index item above node */
            inew = inode_new(above_prev->right, NULL, node, zone);
            above_prev->right = inew;
            node->level = 1;
            if(node->node->level < 1){ node->node->level = 1; }
            above_prev = inode = above = inew;
         }
      }
      prev = node;
      node = next;
      next = next->next;
   }
   return raised;
}

/**
 * bg_raise_ilevel - raise the index levels
 * @iprev      - the first index node at this level
 * @iprev_tall - the first index node at the next highest level
 * @height     - the height of the level we are raising
 * @zone    - NUMA zone
 *
 * Returns 1 if a node was raised and 0 otherwise.
 */
static int bg_raise_ilevel(inode_t *iprev, inode_t *iprev_tall, int height, int zone)
{
   int raised = 0;
   inode_t *index, *inext, *inew, *above, *above_prev;
   above = above_prev = iprev_tall;
   assert(NULL != iprev);
   assert(NULL != iprev_tall);

   index = iprev->right;
   while ((NULL != index) && (NULL != (inext = index->right))) {
      while (index->intermed->marked) {
         /* skip deleted nodes */
         iprev->right = inext;
         if (NULL == inext) break;
         index = inext;
         inext = inext->right;
      }
      if (NULL == inext) break;
      if (((iprev->intermed->level <= height) &&
          (index->intermed->level <= height)) &&
          (inext->intermed->level <= height)) {

         raised = 1;

         /* get the correct index above and behind */
         while (above && above->intermed->key < index->intermed->key) {
            above = above->right;
            if (above != iprev_tall->right) { above_prev = above_prev->right; }
         }

         inew = inode_new(above_prev->right, index, index->intermed, zone);
         above_prev->right = inew;
         index->intermed->level = height + 1;
         if(index->intermed->node->level < height + 1) {
            index->intermed->node->level = height + 1;
         }
         above_prev = above = iprev_tall = inew;
      }
      iprev = index;
      index = inext;
   }
   return raised;
}

/**
 * bg_lower_ilevel - lower the index level
 * @new_low - the first index item in the second lowest level
 * @zone    - NUMA zone
 *
 * Note: the lowest index level is removed by nullifying
 * the reference to the lowest level from the second lowest level.
 */
void bg_lower_ilevel(inode_t *new_low, int zone)
{
   inode_t *old_low = new_low->down;

   /* remove the lowest index level */
   while (NULL != new_low) {
      new_low->down = NULL;
      --new_low->intermed->level;
      if(new_low->intermed->node->level > 0) { --new_low->intermed->node->level; }
      new_low = new_low->right;
   }

   /* garbage collect the old low level */
   while (NULL != old_low) {
      inode_t* next = old_low->right;
      inode_delete(old_low, zone);
      old_low = next;
   }
}

/**
 *  update_index_layer() - updates the index layer based on the local intermediate layer
 *  @obj - the enclave object
 */
void update_index_layer(enclave* obj) {
   inode_t* sentinel = obj->get_sentinel();
   inode_t *inode, *inew;
   inode_t *inodes[MAX_LEVELS];
   int numa_zone = obj->get_numa_zone();
   int raised = 0; /* keep track of if we raised index level */
   int threshold;  /* for testing if we should lower index level */
   int i;
   int non_deleted = 0;
   int tall_deleted = 0;
   for (i = 0; i < MAX_LEVELS; i++) {
      inodes[i] = NULL;
   }

   // traverse the intermediate layer and do physical deletes
   bg_trav_mnodes(obj);

   assert(sentinel->intermed->level < MAX_LEVELS);

   /* get the first index node at each level */
   inode = sentinel;
   for (i = sentinel->intermed->level - 1; i >= 0; i--) {
      inodes[i] = inode;
      assert(NULL != inodes[i]);
      inode = inode->down;
   }
   assert(NULL == inode);

   // raise bottom level nodes
   raised = bg_raise_mlevel(inodes[0]->intermed, inodes[0], numa_zone);

   if (raised && (1 == sentinel->intermed->level)) {
      /* add a new index level */
      sentinel = obj->set_sentinel(inode_new(NULL, sentinel, sentinel->intermed, numa_zone));

      ++sentinel->intermed->level;
      if(sentinel->intermed->node->level < sentinel->intermed->level) {
         sentinel->intermed->node->level = sentinel->intermed->level;
      }
      assert(NULL == inodes[1]);
      inodes[1] = sentinel;
      #ifdef BG_STATS
      ++obj->shadow_stats.raises;
      #endif
   }

   // raise the index level nodes
   for (i = 0; i < (sentinel->intermed->level - 1); i++) {
      assert(i < MAX_LEVELS-1);
      raised = bg_raise_ilevel(inodes[i],    // level raised
                              inodes[i + 1], // level above
                              i + 1,         // current height
                              numa_zone);
   }

   if (raised) {
      // add a new index level
      sentinel = obj->set_sentinel(inode_new(NULL, sentinel, sentinel->intermed, numa_zone));
      ++sentinel->intermed->level;
      if(sentinel->intermed->node->level < sentinel->intermed->level) {
         sentinel->intermed->node->level = sentinel->intermed->level;
      }
      #ifdef BG_STATS
      ++obj->shadow_stats.raises;
      #endif
   }

   // if needed, remove the lowest index level
   if (obj->tall_del > obj->non_del * 10) {
      if (NULL != inodes[1]) {
         bg_lower_ilevel(inodes[1], numa_zone); // level above
         #ifdef BG_STATS
         ++obj->shadow_stats.lowers;
         #endif
      }
   }
}

/**
 * node_remove() - attempts to remove a node from the data layer
 * @prev - the node before the node to be deleted
 * @node - the node we are attempting to delete
 */
void node_remove(node_t* prev, node_t* node) {
   node_t *ptr, *insert;
   assert(prev);
   assert(node);

   if(node->val != node || node->key == 0) return;

   ptr = node->next;
   while(!ptr || ptr->key != 0) {
      // use key = 0 as marker for node to delete
      insert = node_new(0, NULL, node, ptr);
      insert->val = insert;
      CAS(&node->next, ptr, insert);

      assert(node->next != node);
      ptr = node->next; // ptr == insert
   }
   // ensure if key == 0 that it has a previous (so don't count sentinel)
   if(prev->next != node || (prev->key == 0 && prev->prev)) return;
   CAS(&prev->next, node, ptr->next);
   assert(prev->next != prev);
}

/**
 * helper_loop() - defines the execution flow of the helper thread in each enclave
 * @args - the enclave object that owns the helper thread
 */
void* helper_loop(void* args) {
   enclave* obj         = (enclave*)args;
   inode_t* sentinel    = obj->get_sentinel();
   int      numa_zone   = obj->get_numa_zone();
   op_t*    local_job   = new op_t();
   // Pin to CPU
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(obj->get_cpu(), &cpuset);
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

   while(1) {
      if(obj->finished) break;
      if(!obj->index_busy) {
         if(CAS(&obj->index_busy, false, true)) {
            // Helper thread has exclusive access to the index layer
            update_index_layer(obj);
            obj->index_busy = false;
         }
      }
      // Update intermediate layer from op array
      op_t* cur_job = local_job;
      while((cur_job = obj->opbuffer_remove(cur_job))) {
         update_intermediate_layer(obj, cur_job);
      }
      usleep(obj->sleep_time);
   }
   return NULL;
}


/*
 * TODO: Stuff to discuss for design/implementation
 * --> Initial population/resetting of indexes
 * --> My choice to work the enclave opbuffer internal indexes
 * --> Application thread: do we want to choose a key before we get control of mutex?
 * --> Mutex handoff: helper thread is obvious, but what about application thread?
 * --> Helper thread index update (already seems sequential. Do we need mutex?)
 */
