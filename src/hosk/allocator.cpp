/*
 * allocator.cpp: custom NUMA allocator
 *
 * Author: Henry Daly, 2018
 */

/**
 * Module Overview:
 *
 * This is a custom allocator to process allocation requests for HOSK (NUMASK reuse). It
 * services index and intermediate layer node allocation requests. We deploy one instance
 * per (thread). The inherent latency of the OS call in numa_alloc_local (it mmaps per request)
 * practically requires these. Our allocator consists of a linear allocator with three
 * main alterations:
 *    - it can reallocate buffers, if necessary
 *    - allocations are made in a specific NUMA zone
 *    - requests are custom aligned for index and intermediate nodes to fit cache lines
 *
 * A basic linear allocator works as follows: upon initialization, a buffer is allocated.
 * As allocations are requested, the pointer to the first free space is moved forward and
 * the old value is returned.
 */

#include <numa.h>
#include <stdio.h>
#include "allocator.h"
#include "common.h"

/* Constructor */
numa_allocator::numa_allocator(unsigned size_one, unsigned size_two) {
   cache_size = CACHE_LINE_SIZE;
   buf_size[0] = size_one;
   buf_size[1] = size_two;
   empty[0] = empty[1] = last_alloc_half[0] = last_alloc_half[1] = false;
   num_buffers[0] = num_buffers[1] = 0;
   buf_old[0] = buf_old[1] = other_buffers[0] = other_buffers[1] = NULL;
   buf_cur[0] = buf_start[0] = numa_alloc_local(buf_size[0]);
   buf_cur[1] = buf_start[1] = numa_alloc_local(buf_size[1]);
}

/* Destructor */
numa_allocator::~numa_allocator() {
   // free all the buffers
   nreset();
}

/* nalloc() - service allocation request */
void* numa_allocator::nalloc(unsigned ssize, unsigned buf_id) {
   // get cache-line alignment for request
   int alignment = (ssize <= cache_size / 2)? cache_size / 2: cache_size;

   /* if the last allocation was half a cache line and we want a full cache line, we move
      the free space pointer forward a half cache line so we don't spill over cache lines */
   if(last_alloc_half[buf_id] && (alignment == cache_size)) {
      buf_cur[buf_id] = ((char*)buf_cur[buf_id]) + (cache_size / 2);
      last_alloc_half[buf_id] = false;
   }
   else if(!last_alloc_half[buf_id] && (alignment == cache_size / 2)) {
      last_alloc_half[buf_id] = true;
   }
   // get alignment size
   unsigned aligned_size = align(ssize, alignment);

   // reallocate if not enough space left
   if(((char*)buf_cur[buf_id]) + aligned_size > ((char*)buf_start[buf_id]) + buf_size[buf_id]) {
      nrealloc(buf_id);
   }
   // service allocation request
   buf_old[buf_id] = buf_cur[buf_id];
   buf_cur[buf_id] = ((char*)buf_cur[buf_id]) + aligned_size;
   return buf_old[buf_id];
}

/* nfree() - "frees" space (in practice this does nothing unless the allocation was the last request) */
void numa_allocator::nfree(void *ptr, unsigned ssize, unsigned buf_id) {
   // get alignment size
   int alignment = (ssize <= cache_size / 2)? cache_size / 2: cache_size;
   unsigned aligned_size = align(ssize, alignment);

   // only "free" if last allocation
   if(!memcmp(ptr, buf_old[buf_id], aligned_size)) {
      buf_cur[buf_id] = buf_old[buf_id];
      memset(buf_cur[buf_id], 0, aligned_size);
      if(last_alloc_half[buf_id] && (alignment == cache_size / 2)) {
         last_alloc_half[buf_id] = false;
      }
   }
}

/* nreset() - frees all memory buffers */
void numa_allocator::nreset(void) {
   for(unsigned i = 0; i < buf_num; ++i) {
      if(!empty[i]) {
         empty[i] = true;
         // free other_buffers, if used
         if(other_buffers[i] != NULL) {
            int j = num_buffers[i] - 1;
            while(j >= 0) {
               numa_free(other_buffers[i][j], buf_size[i]);
               j--;
            }
            free(other_buffers[i]);
         }
         // free primary buffer
         numa_free(buf_start[i], buf_size[i]);
      }
   }

}

/* nrealloc() - allocates a new buffer */
void numa_allocator::nrealloc(unsigned buf_id) {
   // increase size of our old_buffers to store the previously allocated memory
   printf("Entering realloc(buf %d)!\n", buf_id);
   exit(-1);
   num_buffers[buf_id]++;
   if(other_buffers[buf_id] == NULL) {
      assert(num_buffers[buf_id] == 1);
      other_buffers[buf_id] = (void**)malloc(num_buffers[buf_id] * sizeof(void*));
      *(other_buffers[buf_id]) = buf_start[buf_id];
   } else {
      void** new_bufs = (void**)malloc(num_buffers[buf_id] * sizeof(void*));
      for(int i = 0; i < num_buffers[buf_id] - 1; ++i) {
         new_bufs[i] = other_buffers[buf_id][i];
      }
      new_bufs[num_buffers[buf_id]-1] = buf_start[buf_id];
      free(other_buffers[buf_id]);
      other_buffers[buf_id] = new_bufs;
   }
   // allocate new buffer & update pointers and total size
   buf_cur[buf_id] = buf_start[buf_id] = numa_alloc_local(buf_size[buf_id]);
}

/* align() - gets the aligned size given requested size */
inline unsigned numa_allocator::align(unsigned old, unsigned alignment) {
   return old + ((alignment - (old % alignment))) % alignment;
}
