/*
 * Copyright (c) 2005 Hewlett-Packard Development Company, L.P.
 *
 * This file may be redistributed and/or modified under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2, or (at your option) any later version.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License in the
 * file COPYING for more details.
 */

#if defined(HAVE_CONFIG_H)
# include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define MK_AO_REQUIRE_CAS
#include "atomic_ops_stack.h"

#ifdef MK_AO_USE_ALMOST_LOCK_FREE

  void MK_AO_pause(int); /* defined in atomic_ops.c */

/* LIFO linked lists based on compare-and-swap.  We need to avoid       */
/* the case of a node deletion and reinsertion while I'm deleting       */
/* it, since that may cause my CAS to succeed eventhough the next       */
/* pointer is now wrong.  Our solution is not fully lock-free, but it   */
/* is good enough for signal handlers, provided we have a suitably low  */
/* bound on the number of recursive signal handler reentries.           */
/* A list consists of a first pointer and a blacklist                   */
/* of pointer values that are currently being removed.  No list element */
/* on the blacklist may be inserted.  If we would otherwise do so, we   */
/* are allowed to insert a variant that differs only in the least       */
/* significant, ignored, bits.  If the list is full, we wait.           */

/* Crucial observation: A particular padded pointer x (i.e. pointer     */
/* plus arbitrary low order bits) can never be newly inserted into      */
/* a list while it's in the corresponding auxiliary data structure.     */

/* The second argument is a pointer to the link field of the element    */
/* to be inserted.                                                      */
/* Both list headers and link fields contain "perturbed" pointers, i.e. */
/* pointers with extra bits "or"ed into the low order bits.             */
void
MK_AO_stack_push_explicit_aux_release(volatile MK_AO_t *list, MK_AO_t *x,
                                   MK_AO_stack_aux *a)
{
  MK_AO_t x_bits = (MK_AO_t)x;
  MK_AO_t next;

  /* No deletions of x can start here, since x is not currently in the  */
  /* list.                                                              */
 retry:
# if MK_AO_BL_SIZE == 2
  {
    /* Start all loads as close to concurrently as possible. */
    MK_AO_t entry1 = MK_AO_load(a -> MK_AO_stack_bl);
    MK_AO_t entry2 = MK_AO_load(a -> MK_AO_stack_bl + 1);
    if (entry1 == x_bits || entry2 == x_bits)
      {
        /* Entry is currently being removed.  Change it a little.       */
          ++x_bits;
          if ((x_bits & MK_AO_BIT_MASK) == 0)
            /* Version count overflowed;         */
            /* EXTREMELY unlikely, but possible. */
            x_bits = (MK_AO_t)x;
        goto retry;
      }
  }
# else
  {
    int i;
    for (i = 0; i < MK_AO_BL_SIZE; ++i)
      {
        if (MK_AO_load(a -> MK_AO_stack_bl + i) == x_bits)
          {
            /* Entry is currently being removed.  Change it a little.   */
              ++x_bits;
              if ((x_bits & MK_AO_BIT_MASK) == 0)
                /* Version count overflowed;         */
                /* EXTREMELY unlikely, but possible. */
                x_bits = (MK_AO_t)x;
            goto retry;
          }
      }
  }
# endif
  /* x_bits is not currently being deleted */
  do
    {
      next = MK_AO_load(list);
      *x = next;
    }
  while (MK_AO_EXPECT_FALSE(!MK_AO_compare_and_swap_release(list, next, x_bits)));
}

/*
 * I concluded experimentally that checking a value first before
 * performing a compare-and-swap is usually beneficial on X86, but
 * slows things down appreciably with contention on Itanium.
 * Since the Itanium behavior makes more sense to me (more cache line
 * movement unless we're mostly reading, but back-off should guard
 * against that), we take Itanium as the default.  Measurements on
 * other multiprocessor architectures would be useful.  (On a uniprocessor,
 * the initial check is almost certainly a very small loss.) - HB
 */
#ifdef __i386__
# define PRECHECK(a) (a) == 0 &&
#else
# define PRECHECK(a)
#endif

MK_AO_t *
MK_AO_stack_pop_explicit_aux_acquire(volatile MK_AO_t *list, MK_AO_stack_aux * a)
{
  unsigned i;
  int j = 0;
  MK_AO_t first;
  MK_AO_t * first_ptr;
  MK_AO_t next;

 retry:
  first = MK_AO_load(list);
  if (0 == first) return 0;
  /* Insert first into aux black list.                                  */
  /* This may spin if more than MK_AO_BL_SIZE removals using auxiliary     */
  /* structure a are currently in progress.                             */
  for (i = 0; ; )
    {
      if (PRECHECK(a -> MK_AO_stack_bl[i])
          MK_AO_compare_and_swap_acquire(a->MK_AO_stack_bl+i, 0, first))
        break;
      ++i;
      if ( i >= MK_AO_BL_SIZE )
        {
          i = 0;
          MK_AO_pause(++j);
        }
    }
  assert(i < MK_AO_BL_SIZE);
  assert(a -> MK_AO_stack_bl[i] == first);
  /* First is on the auxiliary black list.  It may be removed by        */
  /* another thread before we get to it, but a new insertion of x       */
  /* cannot be started here.                                            */
  /* Only we can remove it from the black list.                         */
  /* We need to make sure that first is still the first entry on the    */
  /* list.  Otherwise it's possible that a reinsertion of it was        */
  /* already started before we added the black list entry.              */
# if defined(__alpha__) && (__GNUC__ == 4)
    if (first != MK_AO_load(list))
                        /* Workaround __builtin_expect bug found in     */
                        /* gcc-4.6.3/alpha causing test_stack failure.  */
# else
    if (MK_AO_EXPECT_FALSE(first != MK_AO_load(list)))
# endif
  {
    MK_AO_store_release(a->MK_AO_stack_bl+i, 0);
    goto retry;
  }
  first_ptr = MK_AO_REAL_NEXT_PTR(first);
  next = MK_AO_load(first_ptr);
# if defined(__alpha__) && (__GNUC__ == 4)
    if (!MK_AO_compare_and_swap_release(list, first, next))
# else
    if (MK_AO_EXPECT_FALSE(!MK_AO_compare_and_swap_release(list, first, next)))
# endif
  {
    MK_AO_store_release(a->MK_AO_stack_bl+i, 0);
    goto retry;
  }
  assert(*list != first);
  /* Since we never insert an entry on the black list, this cannot have */
  /* succeeded unless first remained on the list while we were running. */
  /* Thus its next link cannot have changed out from under us, and we   */
  /* removed exactly one entry and preserved the rest of the list.      */
  /* Note that it is quite possible that an additional entry was        */
  /* inserted and removed while we were running; this is OK since the   */
  /* part of the list following first must have remained unchanged, and */
  /* first must again have been at the head of the list when the        */
  /* compare_and_swap succeeded.                                        */
  MK_AO_store_release(a->MK_AO_stack_bl+i, 0);
  return first_ptr;
}

#else /* ! USE_ALMOST_LOCK_FREE */

/* Better names for fields in MK_AO_stack_t */
#define ptr MK_AO_val2
#define version MK_AO_val1

#if defined(MK_AO_HAVE_compare_double_and_swap_double)

void MK_AO_stack_push_release(MK_AO_stack_t *list, MK_AO_t *element)
{
    MK_AO_t next;

    do {
      next = MK_AO_load(&(list -> ptr));
      *element = next;
    } while (MK_AO_EXPECT_FALSE(!MK_AO_compare_and_swap_release(&(list -> ptr),
                                                      next, (MK_AO_t)element)));
    /* This uses a narrow CAS here, an old optimization suggested       */
    /* by Treiber.  Pop is still safe, since we run into the ABA        */
    /* problem only if there were both intervening "pop"s and "push"es. */
    /* In that case we still see a change in the version number.        */
}

MK_AO_t *MK_AO_stack_pop_acquire(MK_AO_stack_t *list)
{
#   ifdef __clang__
      MK_AO_t *volatile cptr;
                        /* Use volatile to workaround a bug in          */
                        /* clang-1.1/x86 causing test_stack failure.    */
#   else
      MK_AO_t *cptr;
#   endif
    MK_AO_t next;
    MK_AO_t cversion;

    do {
      /* Version must be loaded first.  */
      cversion = MK_AO_load_acquire(&(list -> version));
      cptr = (MK_AO_t *)MK_AO_load(&(list -> ptr));
      if (cptr == 0) return 0;
      next = *cptr;
    } while (MK_AO_EXPECT_FALSE(!MK_AO_compare_double_and_swap_double_release(list,
                                        cversion, (MK_AO_t)cptr,
                                        cversion+1, (MK_AO_t)next)));
    return cptr;
}


#elif defined(MK_AO_HAVE_compare_and_swap_double)

/* Needed for future IA64 processors.  No current clients? */

#error Untested!  Probably doesnt work.

/* We have a wide CAS, but only does an MK_AO_t-wide comparison.   */
/* We can't use the Treiber optimization, since we only check   */
/* for an unchanged version number, not an unchanged pointer.   */
void MK_AO_stack_push_release(MK_AO_stack_t *list, MK_AO_t *element)
{
    MK_AO_t version;
    MK_AO_t next_ptr;

    do {
      /* Again version must be loaded first, for different reason.      */
      version = MK_AO_load_acquire(&(list -> version));
      next_ptr = MK_AO_load(&(list -> ptr));
      *element = next_ptr;
    } while (!MK_AO_compare_and_swap_double_release(
                           list, version,
                           version+1, (MK_AO_t) element));
}

MK_AO_t *MK_AO_stack_pop_acquire(MK_AO_stack_t *list)
{
    MK_AO_t *cptr;
    MK_AO_t next;
    MK_AO_t cversion;

    do {
      cversion = MK_AO_load_acquire(&(list -> version));
      cptr = (MK_AO_t *)MK_AO_load(&(list -> ptr));
      if (cptr == 0) return 0;
      next = *cptr;
    } while (!MK_AO_compare_double_and_swap_double_release
                    (list, cversion, (MK_AO_t) cptr, cversion+1, next));
    return cptr;
}


#endif /* MK_AO_HAVE_compare_and_swap_double */

#endif /* ! USE_ALMOST_LOCK_FREE */
