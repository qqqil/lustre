/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
#ifndef __LIBCFS_LINUX_KP30_H__
#define __LIBCFS_LINUX_KP30_H__

#ifndef __LIBCFS_KP30_H__
#error Do not #include this file directly. #include <libcfs/kp30.h> instead
#endif

#ifdef __KERNEL__
# include <linux/config.h>
# include <linux/kernel.h>
# include <linux/mm.h>
# include <linux/string.h>
# include <linux/stat.h>
# include <linux/init.h>
# include <linux/errno.h>
# include <linux/unistd.h>
# include <asm/system.h>
# include <linux/kmod.h>
# include <linux/notifier.h>
# include <linux/fs.h>
# include <asm/segment.h>
# include <linux/miscdevice.h>
# include <linux/vmalloc.h>
# include <linux/time.h>
# include <linux/slab.h>
# include <linux/interrupt.h>
# include <linux/highmem.h>
# include <linux/module.h>
# include <linux/version.h>
# include <portals/p30.h>
# include <linux/smp_lock.h>
# include <asm/atomic.h>
# include <asm/uaccess.h>
# include <linux/rwsem.h>
# include <linux/proc_fs.h>
# include <linux/file.h>
# include <linux/smp.h>
# include <linux/ctype.h>
# ifdef HAVE_MM_INLINE
#  include <linux/mm_inline.h>
# endif
# if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
#  include <linux/kallsyms.h>
# endif

#include <libcfs/linux/portals_compat25.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
#define schedule_work schedule_task
#define prepare_work(wq,cb,cbdata)                                            \
do {                                                                          \
        INIT_TQUEUE((wq), 0, 0);                                              \
        PREPARE_TQUEUE((wq), (cb), (cbdata));                                 \
} while (0)

#define PageUptodate Page_Uptodate
#define our_recalc_sigpending(current) recalc_sigpending(current)
#define num_online_cpus() smp_num_cpus
static inline void our_cond_resched(void)
{
        if (current->need_resched)
               schedule ();
}
#define work_struct_t       struct tq_struct

#else

#define prepare_work(wq,cb,cbdata)                                            \
do {                                                                          \
        INIT_WORK((wq), (void *)(cb), (void *)(cbdata));                      \
} while (0)
#define wait_on_page wait_on_page_locked
#define our_recalc_sigpending(current) recalc_sigpending()
#define strtok(a,b) strpbrk(a, b)
static inline void our_cond_resched(void)
{
        cond_resched();
}
#define work_struct_t      struct work_struct

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0) */

#ifdef CONFIG_SMP
#define LASSERT_SPIN_LOCKED(lock) LASSERT(spin_is_locked(lock))
#else
#define LASSERT_SPIN_LOCKED(lock) do {} while(0)
#endif

#ifdef __arch_um__
#define LBUG_WITH_LOC(file, func, line)                                 \
do {                                                                    \
        CEMERG("LBUG - trying to dump log to /tmp/lustre-log\n");       \
        portals_debug_dumplog();                                        \
        portals_run_lbug_upcall(file, func, line);                      \
        panic("LBUG");                                                  \
} while (0)
#else
#define LBUG_WITH_LOC(file, func, line)                                 \
do {                                                                    \
        CEMERG("LBUG\n");                                               \
        portals_debug_dumpstack(NULL);                                  \
        portals_debug_dumplog();                                        \
        portals_run_lbug_upcall(file, func, line);                      \
        set_task_state(current, TASK_UNINTERRUPTIBLE);                  \
        schedule();                                                     \
} while (0)
#endif /* __arch_um__ */

/* ------------------------------------------------------------------- */

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))

#define PORTAL_SYMBOL_REGISTER(x) inter_module_register(#x, THIS_MODULE, &x)
#define PORTAL_SYMBOL_UNREGISTER(x) inter_module_unregister(#x)

#define PORTAL_SYMBOL_GET(x) ((typeof(&x))inter_module_get(#x))
#define PORTAL_SYMBOL_PUT(x) inter_module_put(#x)

#define PORTAL_MODULE_USE       MOD_INC_USE_COUNT
#define PORTAL_MODULE_UNUSE     MOD_DEC_USE_COUNT
#else

#define PORTAL_SYMBOL_REGISTER(x)
#define PORTAL_SYMBOL_UNREGISTER(x)

#define PORTAL_SYMBOL_GET(x) symbol_get(x)
#define PORTAL_SYMBOL_PUT(x) symbol_put(x)

#define PORTAL_MODULE_USE       try_module_get(THIS_MODULE)
#define PORTAL_MODULE_UNUSE     module_put(THIS_MODULE)

#endif

/******************************************************************************/
/* Module parameter support */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
# define CFS_MODULE_PARM(name, t, type, perm, desc) \
        MODULE_PARM(name, t);\
        MODULE_PARM_DESC(name, desc)

# define CFS_SYSFS_MODULE_PARM  0 /* no sysfs module parameters */
#else
# define CFS_MODULE_PARM(name, t, type, perm, desc) \
        module_param(name, type, perm);\
        MODULE_PARM_DESC(name, desc)
# define CFS_SYSFS_MODULE_PARM  1 /* module parameters accessible via sysfs */
#endif
/******************************************************************************/

#if (__GNUC__)
/* Use the special GNU C __attribute__ hack to have the compiler check the
 * printf style argument string against the actual argument count and
 * types.
 */
#ifdef printf
# warning printf has been defined as a macro...
# undef printf
#endif

#endif /* __GNUC__ */

# define fprintf(a, format, b...) CDEBUG(D_OTHER, format , ## b)
# define printf(format, b...) CDEBUG(D_OTHER, format , ## b)
# define time(a) CURRENT_TIME

#else  /* !__KERNEL__ */
# include <stdio.h>
# include <stdlib.h>
#ifndef __CYGWIN__
# include <stdint.h>
#else
# include <cygwin-ioctl.h>
#endif
# include <unistd.h>
# include <time.h>
# include <limits.h>
# include <errno.h>
# include <sys/ioctl.h>                         /* for _IOWR */

# define CFS_MODULE_PARM(name, t, type, perm, desc)

#endif /* End of !__KERNEL__ */

/******************************************************************************/
/* Light-weight trace
 * Support for temporary event tracing with minimal Heisenberg effect. */
#define LWT_SUPPORT  0

#define LWT_MEMORY   (16<<20)

#if !KLWT_SUPPORT
# if defined(__KERNEL__)
#  if !defined(BITS_PER_LONG)
#   error "BITS_PER_LONG not defined"
#  endif
# elif !defined(__WORDSIZE)
#  error "__WORDSIZE not defined"
# else
#  define BITS_PER_LONG __WORDSIZE
# endif

/* kernel hasn't defined this? */
typedef struct {
        long long   lwte_when;
        char       *lwte_where;
        void       *lwte_task;
        long        lwte_p1;
        long        lwte_p2;
        long        lwte_p3;
        long        lwte_p4;
# if BITS_PER_LONG > 32
        long        lwte_pad;
# endif
} lwt_event_t;
#endif /* !KLWT_SUPPORT */

#if LWT_SUPPORT
# ifdef __KERNEL__
#  if !KLWT_SUPPORT

typedef struct _lwt_page {
        struct list_head     lwtp_list;
        struct page         *lwtp_page;
        lwt_event_t         *lwtp_events;
} lwt_page_t;

typedef struct {
        int                lwtc_current_index;
        lwt_page_t        *lwtc_current_page;
} lwt_cpu_t;

extern int       lwt_enabled;
extern lwt_cpu_t lwt_cpus[];

/* Note that we _don't_ define LWT_EVENT at all if LWT_SUPPORT isn't set.
 * This stuff is meant for finding specific problems; it never stays in
 * production code... */

#define LWTSTR(n)       #n
#define LWTWHERE(f,l)   f ":" LWTSTR(l)
#define LWT_EVENTS_PER_PAGE (PAGE_SIZE / sizeof (lwt_event_t))

#define LWT_EVENT(p1, p2, p3, p4)                                       \
do {                                                                    \
        unsigned long    flags;                                         \
        lwt_cpu_t       *cpu;                                           \
        lwt_page_t      *p;                                             \
        lwt_event_t     *e;                                             \
                                                                        \
        if (lwt_enabled) {                                              \
                local_irq_save (flags);                                 \
                                                                        \
                cpu = &lwt_cpus[smp_processor_id()];                    \
                p = cpu->lwtc_current_page;                             \
                e = &p->lwtp_events[cpu->lwtc_current_index++];         \
                                                                        \
                if (cpu->lwtc_current_index >= LWT_EVENTS_PER_PAGE) {   \
                        cpu->lwtc_current_page =                        \
                                list_entry (p->lwtp_list.next,          \
                                            lwt_page_t, lwtp_list);     \
                        cpu->lwtc_current_index = 0;                    \
                }                                                       \
                                                                        \
                e->lwte_when  = get_cycles();                           \
                e->lwte_where = LWTWHERE(__FILE__,__LINE__);            \
                e->lwte_task  = current;                                \
                e->lwte_p1    = (long)(p1);                             \
                e->lwte_p2    = (long)(p2);                             \
                e->lwte_p3    = (long)(p3);                             \
                e->lwte_p4    = (long)(p4);                             \
                                                                        \
                local_irq_restore (flags);                              \
        }                                                               \
} while (0)

#endif /* !KLWT_SUPPORT */

extern int  lwt_init (void);
extern void lwt_fini (void);
extern int  lwt_lookup_string (int *size, char *knlptr,
                               char *usrptr, int usrsize);
extern int  lwt_control (int enable, int clear);
extern int  lwt_snapshot (cycles_t *now, int *ncpu, int *total_size,
                          void *user_ptr, int user_size);
# else  /* __KERNEL__ */
#  define LWT_EVENT(p1,p2,p3,p4)     /* no userland implementation yet */
# endif /* __KERNEL__ */
#endif /* LWT_SUPPORT */

/* ------------------------------------------------------------------ */

#define IOCTL_PORTAL_TYPE long

#ifdef __CYGWIN__
# ifndef BITS_PER_LONG
#  if (~0UL) == 0xffffffffUL
#   define BITS_PER_LONG 32
#  else
#   define BITS_PER_LONG 64
#  endif
# endif
#endif

#if BITS_PER_LONG > 32
# define LI_POISON ((int)0x5a5a5a5a5a5a5a5a)
# define LL_POISON ((long)0x5a5a5a5a5a5a5a5a)
# define LP_POISON ((void *)(long)0x5a5a5a5a5a5a5a5a)
#else
# define LI_POISON ((int)0x5a5a5a5a)
# define LL_POISON ((long)0x5a5a5a5a)
# define LP_POISON ((void *)(long)0x5a5a5a5a)
#endif

#if (defined(__x86_64__) && defined(__KERNEL__))
# define LPU64 "%Lu"
# define LPD64 "%Ld"
# define LPX64 "%#Lx"
# define LPSZ  "%lu"
# define LPSSZ "%ld"
#elif (BITS_PER_LONG == 32 || __WORDSIZE == 32)
# define LPU64 "%Lu"
# define LPD64 "%Ld"
# define LPX64 "%#Lx"
# define LPSZ  "%u"
# define LPSSZ "%d"
#elif (BITS_PER_LONG == 64 || __WORDSIZE == 64)
# define LPU64 "%lu"
# define LPD64 "%ld"
# define LPX64 "%#lx"
# define LPSZ  "%lu"
# define LPSSZ "%ld"
#endif
#ifndef LPU64
# error "No word size defined"
#endif

#endif
