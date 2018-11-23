/*
 * helper.cpp: Defines execution of helper threadss
 *
 * Author: Henry Daly, 2018
 *
 * Built off NUMASK background.cpp (2017)
 */

/**
 * Module Overview:
 *
 * The helper thread loops and updates the intermediate layer from the opbuffer. It
 * then attempts to update the index layer based on a set frequency.
 *
 * NOTE: Index layer updates functions are based on No Hotspot's background.c
 */

#include <assert.h>
#include <atomic_ops.h>
#include <pthread.h>
#include <unistd.h>
#include "common.h"
#include "enclave.h"
#include "skiplist.h"

/**
 * node_remove() - attempts to remove a node from the data layer
 * @prev - the node before the node to be deleted
 * @node - the node we are attempting to delete
 * @enclave_id -  enclave
 */
void node_remove(node_t* prev, node_t* node, int enclave_id) {
   node_t *ptr, *insert;
   assert(prev);
   assert(node);

   if(node->val != node || node->key == 0) return;
   ptr = node->next;
   while(!ptr || ptr->key != 0) {
      // use key = 0 as marker for node to delete
      node_t* lnext = node->local_next;
      node_t* lprev = lnext->local_prev;
      insert = node_new(0, NULL, node, ptr, lprev, lnext, enclave_id);
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
 * bg_remove() - start the node physical removal
 * @prev  - the node before the one to remove
 * @mnode - the node to remove
 * @enclave_id -  enclave
 * returns 1 if deleted, 0 if not
 */
void bg_remove(node_t* prev, node_t* node, int enclave_id) {
   if(node->level == 0 && node->val == NULL) {
      // Only remove short nodes
      CAS(&node->val, NULL, node);
      if(node->val == node) {
         node_remove(prev, node, enclave_id);
      }
   }
}

/**
 * bg_trav_nodes - traverse enclave-local nodes and remove if possible
 * @obj - the enclave object for reference
 */
static void bg_trav_nodes(enclave* obj) {
   node_t* prev   = obj->get_sentinel()->node;
   node_t* node   = prev->local_next;
   int enclave_id = obj->get_enclave_num();
#ifdef ADDRESS_CHECKING
   zone_access_check(zone, prev, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
   zone_access_check(zone, node, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif

   while (NULL != node) {
      bg_remove(prev, node, enclave_id);
      if(NULL != node->val && node != node->val) { ++obj->non_del; }
      else if (node->level >= 1)                 { ++obj->tall_del; }
      prev = node;
      node = node->local_next;
#ifdef ADDRESS_CHECKING
      zone_access_check(zone, node, &obj->bg_local_accesses, &obj->bg_foreign_accesses, obj->index_ignore);
#endif
   }
}

/**
 * bg_raise_nlevel - raise level 0 nodes into index levels
 * @inode - starting index node at bottom layer
 * @enclave_id - enclave */
static int bg_raise_nlevel(inode_t* inode, int enclave_id) {
   int raised = 0;
   node_t *prev, *node, *next;
   inode_t *inew, *above, *above_prev;
   above = above_prev = inode;
   assert(NULL != inode);

   prev = inode->node;
   node = node->local_next;
   if (NULL == node) return 0;

   next = node->local_next;
   while (NULL != next) {
      /* don't raise deleted nodes */
      if (node != node->val) {
         if (((prev->level == 0) && (node->level == 0)) && (next->level == 0)) {
            raised = 1;

            /* get the correct index above and behind */
            while (above && above->node->key < node->key) {
               above = above->right;
               if (above != inode->right) { above_prev = above_prev->right; }
            }

            /* add a new index item above node */
            inew = inode_new(above_prev->right, NULL, node, enclave_id);
            above_prev->right = inew;
            node->level = 1;
            above_prev = inode = above = inew;
         }
      }
      prev = node;
      node = next;
      next = next->local_next;
   }
   return raised;
}

/**
 * bg_raise_ilevel - raise the index levels
 * @iprev      - the first index node at this level
 * @iprev_tall - the first index node at the next highest level
 * @height     - the height of the level we are raising
 * @enclave_id -  enclave
 *
 * Returns 1 if a node was raised and 0 otherwise.
 */
static int bg_raise_ilevel(inode_t *iprev, inode_t *iprev_tall, int height, int enclave_id) {
   int raised = 0;
   inode_t *index, *inext, *inew, *above, *above_prev;
   above = above_prev = iprev_tall;
   assert(NULL != iprev);
   assert(NULL != iprev_tall);

   index = iprev->right;
   while ((NULL != index) && (NULL != (inext = index->right))) {
      while (index->node->val == index->node) {
         /* skip deleted nodes */
         iprev->right = inext;
         if (NULL == inext) break;
         index = inext;
         inext = inext->right;
      }
      if (NULL == inext) break;
      if (((iprev->node->level <= height) &&
          (index->node->level <= height)) &&
          (inext->node->level <= height)) {

         raised = 1;

         /* get the correct index above and behind */
         while (above && above->node->key < index->node->key) {
            above = above->right;
            if (above != iprev_tall->right) { above_prev = above_prev->right; }
         }

         inew = inode_new(above_prev->right, index, index->node, enclave_id);
         above_prev->right = inew;
         index->node->level = height + 1;
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
 * @enclave_id - enclave
 *
 * Note: the lowest index level is removed by nullifying
 * the reference to the lowest level from the second lowest level.
 */
void bg_lower_ilevel(inode_t *new_low, int enclave_id) {
   inode_t *old_low = new_low->down;

   /* remove the lowest index level */
   while (NULL != new_low) {
      new_low->down = NULL;
      --new_low->node->level;
      // TODO: level considerations
      //if(new_low->intermed->node->level > 0) { --new_low->intermed->node->level; }
      new_low = new_low->right;
   }

   /* garbage collect the old low level */
   while (NULL != old_low) {
      inode_t* next = old_low->right;
      inode_delete(old_low, enclave_id);
      old_low = next;
   }
}

/**
 * helper_loop() - defines the execution flow of the helper thread in each enclave
 * @args - the enclave object that owns the helper thread
 */
void* helper_loop(void* args) {
   enclave* obj         = (enclave*)args;
   // Pin to CPU
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(obj->get_thread_id(HLP_IDX), &cpuset);
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

   if(obj->reset_index) {
      obj->reset_index = false;
      obj->set_sentinel(inode_new(NULL, NULL, obj->get_sentinel()->node, obj->get_enclave_num()));
   }

   while(1) {
      if(obj->finished) break;
      usleep(obj->sleep_time);

      int non_deleted = 0;
      int tall_deleted = 0;
      inode_t* sentinel = obj->get_sentinel();
      inode_t *inode, *inew;
      inode_t *inodes[MAX_LEVELS];
      int enclave_id = obj->get_enclave_num();
      int raised = 0; /* keep track of if we raised index level */
      int threshold;  /* for testing if we should lower index level */
      int i;
      for (i = 0; i < MAX_LEVELS; i++) {
         inodes[i] = NULL;
      }

      // traverse the data layer and do physical deletes
      bg_trav_nodes(obj);

      assert(sentinel->node->level < MAX_LEVELS);

      /* get the first index node at each level */
      inode = sentinel;
      for (i = sentinel->node->level - 1; i >= 0; i--) {
         inodes[i] = inode;
         assert(NULL != inodes[i]);
         inode = inode->down;
      }
      assert(NULL == inode);

      // raise bottom level nodes
      raised = bg_raise_nlevel(inodes[0], enclave_id);

      if (raised && (1 == sentinel->node->level)) {
         /* add a new index level */
         sentinel = obj->set_sentinel(inode_new(NULL, sentinel, sentinel->node, enclave_id));

         ++sentinel->node->level;
         assert(NULL == inodes[1]);
         inodes[1] = sentinel;
         #ifdef BG_STATS
         ++obj->shadow_stats.raises;
         #endif
      }

      // raise the index level nodes
      for (i = 0; i < (sentinel->node->level - 1); i++) {
         assert(i < MAX_LEVELS-1);
         raised = bg_raise_ilevel(inodes[i],    // level raised
                                 inodes[i + 1], // level above
                                 i + 1,         // current height
                                 enclave_id);
      }

      if (raised) {
         // add a new index level
         sentinel = obj->set_sentinel(inode_new(NULL, sentinel, sentinel->node, enclave_id));
         ++sentinel->node->level;
         #ifdef BG_STATS
         ++obj->shadow_stats.raises;
         #endif
      }

      // if needed, remove the lowest index level
      if (obj->tall_del > obj->non_del * 10) {
         if (NULL != inodes[1]) {
            bg_lower_ilevel(inodes[1], enclave_id); // level above
            #ifdef BG_STATS
            ++obj->shadow_stats.lowers;
            #endif
         }
      }
   }
   return NULL;
}
