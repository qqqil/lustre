/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 * Author: Nikita Danilov <nikita@clusterfs.com>
 *
 * This file is part of Lustre, http://www.lustre.org.
 *
 * Lustre is free software; you can redistribute it and/or modify it under the
 * terms of version 2 of the GNU General Public License as published by the
 * Free Software Foundation.
 *
 * Lustre is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Lustre; if not, write to the Free Software Foundation, Inc., 675 Mass
 * Ave, Cambridge, MA 02139, USA.
 *
 * Implementation of portable time API for user-level.
 *
 */

#ifndef __LIBCFS_USER_TIME_H__
#define __LIBCFS_USER_TIME_H__

#ifndef __LIBCFS_LIBCFS_H__
#error Do not #include this file directly. #include <libcfs/libcfs.h> instead
#endif

/* Portable time API */

/*
 * Platform provides three opaque data-types:
 *
 *  cfs_time_t        represents point in time. This is internal kernel
 *                    time rather than "wall clock". This time bears no
 *                    relation to gettimeofday().
 *
 *  cfs_duration_t    represents time interval with resolution of internal
 *                    platform clock
 *
 *  cfs_fs_time_t     represents instance in world-visible time. This is
 *                    used in file-system time-stamps
 *
 *  cfs_time_t     cfs_time_current(void);
 *  cfs_time_t     cfs_time_add    (cfs_time_t, cfs_duration_t);
 *  cfs_duration_t cfs_time_sub    (cfs_time_t, cfs_time_t);
 *  int            cfs_time_before (cfs_time_t, cfs_time_t);
 *  int            cfs_time_beforeq(cfs_time_t, cfs_time_t);
 *
 *  cfs_duration_t cfs_duration_build(int64_t);
 *
 *  time_t         cfs_duration_sec (cfs_duration_t);
 *  void           cfs_duration_usec(cfs_duration_t, struct timeval *);
 *  void           cfs_duration_nsec(cfs_duration_t, struct timespec *);
 *
 *  void           cfs_fs_time_current(cfs_fs_time_t *);
 *  time_t         cfs_fs_time_sec    (cfs_fs_time_t *);
 *  void           cfs_fs_time_usec   (cfs_fs_time_t *, struct timeval *);
 *  void           cfs_fs_time_nsec   (cfs_fs_time_t *, struct timespec *);
 *  int            cfs_fs_time_before (cfs_fs_time_t *, cfs_fs_time_t *);
 *  int            cfs_fs_time_beforeq(cfs_fs_time_t *, cfs_fs_time_t *);
 *
 *  cfs_duration_t cfs_time_minimal_timeout(void)
 *
 *  CFS_TIME_FORMAT
 *  CFS_DURATION_FORMAT
 *
 */

#define ONE_BILLION ((u_int64_t)1000000000)
#define ONE_MILLION ((u_int64_t)   1000000)

#ifndef __KERNEL__

/*
 * Liblustre. time(2) based implementation.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

typedef time_t cfs_fs_time_t;
typedef time_t cfs_time_t;
typedef long cfs_duration_t;

static inline cfs_time_t cfs_time_current(void)
{
        return time(NULL);
}

static inline cfs_duration_t cfs_time_seconds(int seconds)
{
        return seconds;
}

static inline time_t cfs_time_current_sec(void)
{
        return cfs_time_seconds(cfs_time_current());
}

static inline int cfs_time_before(cfs_time_t t1, cfs_time_t t2)
{
        return t1 < t2;
}

static inline int cfs_time_beforeq(cfs_time_t t1, cfs_time_t t2)
{
        return t1 <= t2;
}

static inline cfs_duration_t cfs_duration_build(int64_t nano)
{
        return nano / ONE_BILLION;
}

static inline time_t cfs_duration_sec(cfs_duration_t d)
{
        return d;
}

static inline void cfs_duration_usec(cfs_duration_t d, struct timeval *s)
{
        s->tv_sec = d;
        s->tv_usec = 0;
}

static inline void cfs_duration_nsec(cfs_duration_t d, struct timespec *s)
{
        s->tv_sec = d;
        s->tv_nsec = 0;
}

static inline void cfs_fs_time_current(cfs_fs_time_t *t)
{
        time(t);
}

static inline time_t cfs_fs_time_sec(cfs_fs_time_t *t)
{
        return *t;
}

static inline void cfs_fs_time_usec(cfs_fs_time_t *t, struct timeval *v)
{
        v->tv_sec = *t;
        v->tv_usec = 0;
}

static inline void cfs_fs_time_nsec(cfs_fs_time_t *t, struct timespec *s)
{
        s->tv_sec = *t;
        s->tv_nsec = 0;
}

static inline int cfs_fs_time_before(cfs_fs_time_t *t1, cfs_fs_time_t *t2)
{
        return *t1 < *t2;
}

static inline int cfs_fs_time_beforeq(cfs_fs_time_t *t1, cfs_fs_time_t *t2)
{
        return *t1 <= *t2;
}

static inline cfs_duration_t cfs_time_minimal_timeout(void)
{
        return 1;
}

#define CFS_MIN_DELAY           (1)

static inline cfs_time_t cfs_time_add(cfs_time_t t, cfs_duration_t d)
{
        return t + d;
}

static inline cfs_duration_t cfs_time_sub(cfs_time_t t1, cfs_time_t t2)
{
        return t1 - t2;
}

#define CFS_TIME_T              "%lu"
#define CFS_DURATION_T          "%ld"

/* !__KERNEL__ */
#endif

/* __LIBCFS_USER_TIME_H__ */
#endif
/*
 * Local variables:
 * c-indentation-style: "K&R"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 80
 * scroll-step: 1
 * End:
 */
