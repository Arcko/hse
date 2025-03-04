/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2021 Micron Technology, Inc.  All rights reserved.
 */

#ifndef HSE_PLATFORM_TIMER_H
#define HSE_PLATFORM_TIMER_H

#include <hse/util/arch.h>
#include <hse/util/atomic.h>
#include <hse/error/merr.h>
#include <hse/util/time.h>
#include <hse/util/list.h>

struct timer_jclock {
    volatile ulong jc_jclock_ns HSE_ACP_ALIGNED;
    volatile ulong jc_jiffies;
    ulong          jc_cstart;
    ulong          jc_tstart;
};

struct timer_list {
    struct list_head entry;
    unsigned long    expires;

    void (*function)(unsigned long);
    unsigned long data;
};

/* jiffies is updated HSE_HZ times per second and reflects
 * the time of CLOCK_MONOTONIC divided by HSE_HZ.
 *
 * jclock_ns is updated HSE_HZ times per second and reflects
 * the time of CLOCK_MONOTONIC in nanoseconds.
 */
extern struct timer_jclock timer_jclock;

#define jclock_ns   (timer_jclock.jc_jclock_ns)
#define jiffies     (timer_jclock.jc_jiffies)

extern unsigned long timer_slack;

static HSE_ALWAYS_INLINE unsigned long
msecs_to_jiffies(const unsigned int m)
{
    if ((int)m < 0)
        return MAX_JIFFY_OFFSET;

    return (m + (MSEC_PER_SEC / HSE_HZ) - 1) / (MSEC_PER_SEC / HSE_HZ);
}

static HSE_ALWAYS_INLINE unsigned long
usecs_to_jiffies(const unsigned int m)
{
    if ((int)m < 0)
        return MAX_JIFFY_OFFSET;

    return (m + (USEC_PER_SEC / HSE_HZ) - 1) / (USEC_PER_SEC / HSE_HZ);
}

static HSE_ALWAYS_INLINE unsigned long
nsecs_to_jiffies(const u64 m)
{
    return (m + (NSEC_PER_SEC / HSE_HZ) - 1) / (NSEC_PER_SEC / HSE_HZ);
}

#define init_timer(_timer) INIT_LIST_HEAD(&(_timer)->entry)

#define setup_timer(_timer, _func, _data) \
    do {                                  \
        init_timer((_timer));             \
        (_timer)->function = (_func);     \
        (_timer)->data = (ulong)(_data);  \
    } while (0)

/**
 * add_timer() - Put an initialized timer on the active list
 * @timer, struct timer_list *, timer to be added to the active list.
 */
void
add_timer(struct timer_list *timer);

/**
 * del_timer() - Take a timer off of the active list
 * @timer, struct timer_list *, timer to be removed from the active list
 *
 * Return: 0 if the timer was not on the active list, 1 otherwise.
 */
int
del_timer(struct timer_list *timer);

merr_t hse_timer_init(void) HSE_COLD;
void hse_timer_fini(void) HSE_COLD;

#endif /* HSE_PLATFORM_TIMER_H */
