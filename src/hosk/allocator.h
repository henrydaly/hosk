/*
 * Interface for custom allocator
 *
 *  Author: Henry Daly, 2018
 */
#ifndef NUMA_ALLOCATOR_H_
#define NUMA_ALLOCATOR_H_

#include <stdlib.h>
#define buf_num   2
class numa_allocator {
private:
   void*    buf_start[buf_num];
   unsigned buf_size[buf_num];
   void*    buf_cur[buf_num];
   bool     empty[buf_num];
   void*    buf_old[buf_num];
   unsigned cache_size;

   void**   other_buffers[buf_num];    // for keeping track of
   unsigned num_buffers[buf_num];      // the other buffers

   bool     last_alloc_half[buf_num];  // for half cache line alignment

   void nrealloc(unsigned buf_id);
   void nreset(void);
   inline unsigned align(unsigned old, unsigned alignment);

public:
   numa_allocator(unsigned size_one, unsigned size_two);
   ~numa_allocator();
   void* nalloc(unsigned size, unsigned buf_id);
   void nfree(void *ptr, unsigned size, unsigned buf_id);
};

#endif /* ALLOCATOR_H_ */
