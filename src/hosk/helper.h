/*
 * Interface for the helper thread functions.
 *
 * Author: Henry Daly, 2018
 */
#ifndef HELPER_H_
#define HELPER_H_
#include "skiplist.h"

/* Public helper thread interface */
void  node_remove(node_t* prev, node_t* node);
void* helper_loop(void* args);

#endif /* HELPER_H_ */
