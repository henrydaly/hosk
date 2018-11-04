/*
 * common definitions shared by all modules
 */
#ifndef COMMON_H_
#define COMMON_H_

#include <atomic_ops.h>
#include <pthread.h>
#include "tm.h"

#define VOLATILE /* volatile */
#define BARRIER() asm volatile("" ::: "memory");

#define CAS(_m, _o, _n) \
    AO_compare_and_swap_full(((volatile AO_t*) _m), ((AO_t) _o), ((AO_t) _n))

#define FAI(a) AO_fetch_and_add_full((volatile AO_t*) (a), 1)
#define FAD(a) AO_fetch_and_add_full((volatile AO_t*) (a), -1)
/*
 * Allow us to efficiently align and pad structures so that shared fields
 * don't cause contention on thread-local or read-only fields.
 */
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]
#define ALIGNED_ALLOC(_s)                                       \
    ((void *)(((unsigned long)malloc((_s)+CACHE_LINE_SIZE*2) +  \
        CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE-1)))

#define CACHE_LINE_SIZE 64

typedef struct barrier {
   pthread_cond_t complete;
   pthread_mutex_t mutex;
   int count;
   int crossing;
} barrier_t;

/*
 * Returns a pseudo-random value in [1;range).
 * Depending on the symbolic constant RAND_MAX>=32767 defined in stdlib.h,
 * the granularity of rand() could be lower-bounded by the 32767^th which might
 * be too high for given values of range and initial.
 *
 * Note: this is not thread-safe and will introduce futex locks
 */
inline long rand_range(long r) {
   int m = RAND_MAX;
   int d, v = 0;
   do {
      d = (m > r ? r : m);
      v += 1 + (int)(d * ((double)rand()/((double)(m)+1.0)));
      r -= m;
   } while (r > 0);
   return v;
}
inline long rand_range(long r);

/* Thread-safe, re-entrant version of rand_range(r) */
inline long rand_range_re(unsigned int *seed, long r) {
   int m = RAND_MAX;
   int d, v = 0;
   do {
      d = (m > r ? r : m);
      v += 1 + (int)(d * ((double)rand_r(seed)/((double)(m)+1.0)));
      r -= m;
   } while (r > 0);
   return v;
}
long rand_range_re(unsigned int *seed, long r);

#endif /* COMMON_H_ */
