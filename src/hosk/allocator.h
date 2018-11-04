/*
 * Interface for custom allocator
 *
 *  Author: Henry Daly, 2018
 */
#ifndef NUMA_ALLOCATOR_H_
#define NUMA_ALLOCATOR_H_

#include <stdlib.h>

class numa_allocator {
private:
   void*    buf_start;
   unsigned buf_size;
   void*    buf_cur;
   bool     empty;
   void*    buf_old;
   unsigned cache_size;

   void**   other_buffers;    // for keeping track of
   unsigned num_buffers;      // the other buffers

   bool     last_alloc_half;  // for half cache line alignment

   void nrealloc(void);
   void nreset(void);
   inline unsigned align(unsigned old, unsigned alignment);

public:
   numa_allocator(unsigned ssize);
   ~numa_allocator();
   void* nalloc(unsigned size);
   void nfree(void *ptr, unsigned size);
};

#endif /* ALLOCATOR_H_ */
