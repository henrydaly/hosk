/*
 * Header for all application related functions
 *
 * Author: Henry Daly, 2018
 *
 * NOTE: Some functions courtesy of Synchrobench (V. Gramoli, 2013)
 */

#ifndef APPLICATION_H_
#define APPLICATION_H_

#include <pthread.h>
#include "common.h"
#include "enclave.h"

enum sl_optype { CONTAINS, DELETE, INSERT };
typedef enum sl_optype sl_optype_t;
unsigned int global_seed;
pthread_key_t rng_seed_key;
unsigned int levelmax;

void barrier_init(barrier_t *b, int n)
{
   pthread_cond_init(&b->complete, NULL);
   pthread_mutex_init(&b->mutex, NULL);
   b->count = n;
   b->crossing = 0;
}

void barrier_cross(barrier_t *b)
{
   pthread_mutex_lock(&b->mutex);
   /* One more thread through */
   b->crossing++;
   /* If not all here, wait */
   if (b->crossing < b->count) {
      pthread_cond_wait(&b->complete, &b->mutex);
   } else {
      pthread_cond_broadcast(&b->complete);
      /* Reset for next time */
      b->crossing = 0;
   }
   pthread_mutex_unlock(&b->mutex);
}

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

/* update_results() - update the results structure */
int update_results(sl_optype_t otype, app_res* ares, int result, int key, int old_last, int alternate) {
   int last = old_last;
   switch (otype) {
      case CONTAINS:
         ares->contains++;
         if(result == 1) ares->found++;
         break;
      case INSERT:
         ares->add++;
         if(result == 1) {
            ares->added++;
            last = key;
         }
         break;
      case DELETE:
         ares->remove++;
         if(alternate) last = -1;
         if(result == 1) {
            ares->removed++;
            last = -1;
         }
         break;
      default: break;   // This should never happen
   }
   return last;
}

/* get_unext() - determine what the next operation will be */
inline int get_unext(app_params* d, app_res* r) {
   int result;
   if(d->effective){ // A failed insert/delete is counted as a read-only tx
      result = ((100 * (r->added + r->removed)) < (d->update * (r->add + r->remove + r->contains)));
   } else {          // A failed insert/delete is counted as an update
      result = (rand_range_re(&d->seed, 100) - 1 < d->update);
   }

   return result;
}


#endif /* APPLICATION_H_ */
