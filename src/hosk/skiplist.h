/*
 * Interface for the skip list data structure.
 *
 * Author: Henry Daly, 2017
 * Based on No Hostpot's skiplist.h
 */
#ifndef SKIPLIST_H_
#define SKIPLIST_H_

#include <atomic_ops.h>
#include "common.h"
#define MAX_LEVELS   128
#define NUM_LEVELS   2
#define NODE_LEVEL   0
#define INODE_LEVEL  1
// Uncomment to allow address checking (determines NUMA local accesses) - reduces performance
//#define ADDRESS_CHECKING

typedef unsigned long sl_key_t;
typedef void* val_t;
typedef unsigned int uint;

/* data layer nodes */
struct sl_node {
   struct sl_node*   prev;
   struct sl_node*   next;
   val_t             val;
   sl_key_t          key;
   volatile uint     level;
};

/* index layer nodes */
struct sl_inode {
   struct sl_inode*  right;
   struct sl_inode*  down;
   struct sl_mnode*  intermed;
   sl_key_t          key;
};

/* intermediate layer nodes */
struct sl_mnode {
   struct sl_mnode*  next;
   struct sl_node*   node;
   sl_key_t          key;
   unsigned int      level;
   bool              marked;
};

typedef VOLATILE struct sl_node  node_t;
typedef VOLATILE struct sl_inode inode_t;
typedef VOLATILE struct sl_mnode mnode_t;

node_t* node_new(sl_key_t key, val_t val, node_t *prev, node_t *next);
inode_t* inode_new(inode_t *right, inode_t *down, mnode_t* intermed, int cpu);
mnode_t* mnode_new(mnode_t* next, node_t* node, unsigned int level, int cpu);

void node_delete(node_t *node);
void inode_delete(inode_t *inode, int cpu);
void mnode_delete(mnode_t* mnode, int cpu);
int data_layer_size(node_t* head, int flag);
int intermed_layer_size(mnode_t* head);


#ifdef ADDRESS_CHECKING
   int check_addr(int supposed_node, void* addr);
   void zone_access_check(int supposed_node, void* addr, volatile long* local, volatile long* foreign, bool dont_count);
#endif
#endif /* SKIPLIST_H_ */
