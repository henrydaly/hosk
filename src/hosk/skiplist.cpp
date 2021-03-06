/*
 * skiplist.cpp: definitions of the skip list data structure, updated to use custom allocator
 *
 * Author: Henry Daly, 2017
 *
 * Based on No Hotspot Skip List skiplist.c (2013)
 */

#include <numaif.h>
#include <stdio.h>
#include <stdlib.h>
#include "allocator.h"
#include "common.h"
#include "skiplist.h"

numa_allocator** allocators;
extern bool base_malloc;

#define NODE_SZ   sizeof(node_t)
#define INODE_SZ  sizeof(inode_t)
#define MNODE_SZ  sizeof(mnode_t)

/* initial_populate serves as a manner in which initial population of the
   index layer (which will be scrapped at end of populate) will not waste
   allocator space */

/* - Public skiplist interface - */
/**
 * node_new() - create a new data layer node
 * @key  - the key for the new node
 * @val  - the val for the new node
 * @prev - the prev node pointer for the new node
 * @next - the next node pointer for the new node
 */
node_t* node_new(sl_key_t key, val_t val, node_t *prev, node_t *next) {
   node_t *node;
   node = (node_t*)malloc(NODE_SZ);
   node->key   = key;
   node->val   = val;
   node->prev  = prev;
   node->next  = next;
   node->level = 0;
   return node;
}

/**
 * inode_new() - create a new index node
 * @right    - the right inode pointer for the new inode
 * @down  - the down inode pointer for the new inode
 * @node  - the node pointer for the new inode
 * @intermed - intermediate layer node
 * @enclave_id  -  the enclave number
 */
inode_t* inode_new(inode_t *right, inode_t *down, mnode_t* intermed, int enclave_id) {
   inode_t *inode;
   numa_allocator* local = allocators[enclave_id];
   if(base_malloc) {
       inode = (inode_t*)malloc(INODE_SZ);
       inode->right      = right;
       inode->down       = down;
       inode->intermed   = intermed;
       inode->key        = intermed->key;
       return inode;
   }
   inode = (inode_t*)local->nalloc(INODE_SZ);
   inode->right      = right;
   inode->down       = down;
   inode->intermed   = intermed;
   inode->key        = intermed->key;
   return inode;
}

/**
 * mnode_new() - create a new intermediate node
 * @next  - the right mnode pointer for the new mnode
 * @node  - the data layer node for the the new mnode
 * @level - the starting level of the new mnode
 * @enclave_id  -  the enclave number
 */
mnode_t* mnode_new(mnode_t* next, node_t* node, unsigned int level, int enclave_id) {
   mnode_t* mnode;
   numa_allocator* local = allocators[enclave_id];
   mnode = (mnode_t*)local->nalloc(MNODE_SZ);
   mnode->level   = level;
   mnode->key     = node->key;
   mnode->next    = next;
   mnode->marked  = false;
   mnode->node    = node;
   return mnode;
}

/**
 * node_delete() - delete a data layer node
 * @node - the node to delete
 */
void node_delete(node_t *node) {
   free((void*)node);
}

/**
 * inode_delete() - delete an index layer node
 * @inode - the index node to delete
 * @enclave_id  -  the enclave number
 */
void inode_delete(inode_t *inode, int enclave_id) {
   numa_allocator* local = allocators[enclave_id];
   local->nfree(inode, INODE_SZ);
}

/**
 * mnode_delete() - delete an intermediate node
 * @mnode - the intermediate node to delete
 * @enclave_id  -  the enclave number
 */
void mnode_delete(mnode_t* mnode, int enclave_id) {
   numa_allocator* local = allocators[enclave_id];
   local->nfree(mnode, MNODE_SZ);
}

/**
 * data_layer_size() - returns the size of the data layer
 * @head - the sentinel node for the data layer
 * @flag - specifies if we include logically deleted nodes
 *
 */
int data_layer_size(node_t* head, int flag) {
   struct sl_node *node = head;
   int size = 0;
   node = node->next;
   while (NULL != node) {
      if (flag && (NULL != node->val && node != node->val)) {
         ++size;
      } else if (!flag && node->key != 0) {
         ++size;
      }
      node = node->next;
   }
   return size;
}

int intermed_layer_size(mnode_t* head) {
   int size = 0;
   mnode_t* m = head;
   while(m) {
      if(!m->marked) size++;
      m = m->next;
   }
   return size;
}

#ifdef ADDRESS_CHECKING
/**
 * check_addr() - check specific address using get_mempolicy to see if it is on the supposed node
 */
int check_addr(int supposed_node, void* addr) {
   if(!addr) return -1;

   int actual_node = -1;
   if(-1 == get_mempolicy(&actual_node, NULL, 0, (void*)addr, MPOL_F_NODE | MPOL_F_ADDR)) {
      perror("get_mempolicy error");
      exit(-9);
   }
   if(actual_node == supposed_node) return 0;
   else                             return 1;
}

/**
 * zone_access_check() - checks address and updates local or foreign accesses accordingly
 */
void zone_access_check(int node, void* addr, volatile long* local, volatile long* foreign, bool dont_count) {
   int result;
   if(1 == (result = check_addr(node, addr))) {
      (*foreign)++;
      if(dont_count){ (*foreign)--; }
   } else if(result == 0) {
      (*local)++;
   }
}
#endif
