/*
 * Copyright 1988, 1989 Hans-J. Boehm, Alan J. Demers
 * Copyright (c) 1991-1995 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996-1999 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1999 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose,  provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

/*
 * Copyright (c) 2014, Jean-Claude Beaudoin.
 *
 * On native Microsoft Windows, function MK_GC_get_stack_base() has been replaced.
 */

#include "private/gc_priv.h"

#if defined(LINUX) && !defined(POWERPC) && !defined(NO_SIGCONTEXT_H)
# include <linux/version.h>
# if (LINUX_VERSION_CODE <= 0x10400)
    /* Ugly hack to get struct sigcontext_struct definition.  Required  */
    /* for some early 1.3.X releases.  Will hopefully go away soon.     */
    /* in some later Linux releases, asm/sigcontext.h may have to       */
    /* be included instead.                                             */
#   define __KERNEL__
#   include <asm/signal.h>
#   undef __KERNEL__
# else
    /* Kernels prior to 2.1.1 defined struct sigcontext_struct instead of */
    /* struct sigcontext.  libc6 (glibc2) uses "struct sigcontext" in     */
    /* prototypes, so we have to include the top-level sigcontext.h to    */
    /* make sure the former gets defined to be the latter if appropriate. */
#   include <features.h>
#   if 2 <= __GLIBC__
#     if 2 == __GLIBC__ && 0 == __GLIBC_MINOR__
        /* glibc 2.1 no longer has sigcontext.h.  But signal.h          */
        /* has the right declaration for glibc 2.1.                     */
#       include <sigcontext.h>
#     endif /* 0 == __GLIBC_MINOR__ */
#   else /* __GLIBC__ < 2 */
      /* libc5 doesn't have <sigcontext.h>: go directly with the kernel   */
      /* one.  Check LINUX_VERSION_CODE to see which we should reference. */
#     include <asm/sigcontext.h>
#   endif /* __GLIBC__ < 2 */
# endif
#endif /* LINUX && !POWERPC */

#if !defined(OS2) && !defined(PCR) && !defined(AMIGA) && !defined(MACOS) \
    && !defined(MSWINCE) && !defined(__CC_ARM)
# include <sys/types.h>
# if !defined(MSWIN32)
#   include <unistd.h>
# endif
#endif

#include <stdio.h>
#if defined(MSWINCE) || defined(SN_TARGET_PS3)
# define SIGSEGV 0 /* value is irrelevant */
#else
# include <signal.h>
#endif

#if defined(UNIX_LIKE) || defined(CYGWIN32) || defined(NACL) \
    || defined(SYMBIAN)
# include <fcntl.h>
#endif

#if defined(LINUX) || defined(LINUX_STACKBOTTOM)
# include <ctype.h>
#endif

/* Blatantly OS dependent routines, except for those that are related   */
/* to dynamic loading.                                                  */

#ifdef AMIGA
# define MK_GC_AMIGA_DEF
# include "extra/AmigaOS.c"
# undef MK_GC_AMIGA_DEF
#endif

#if defined(MSWIN32) || defined(MSWINCE) || defined(CYGWIN32)
# ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN 1
# endif
# define NOSERVICE
# include <windows.h>
  /* It's not clear this is completely kosher under Cygwin.  But it     */
  /* allows us to get a working MK_GC_get_stack_base.                      */
#endif

#ifdef MACOS
# include <Processes.h>
#endif

#ifdef IRIX5
# include <sys/uio.h>
# include <malloc.h>   /* for locking */
#endif

#if defined(LINUX) || defined(FREEBSD) || defined(SOLARIS) || defined(IRIX5) \
    || ((defined(USE_MMAP) || defined(USE_MUNMAP)) && !defined(USE_WINALLOC))
# define MMAP_SUPPORTED
#endif

#if defined(MMAP_SUPPORTED) || defined(ADD_HEAP_GUARD_PAGES)
# if defined(USE_MUNMAP) && !defined(USE_MMAP)
#   error "invalid config - USE_MUNMAP requires USE_MMAP"
# endif
# include <sys/types.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <errno.h>
#endif

#ifdef DARWIN
  /* for get_etext and friends */
# include <mach-o/getsect.h>
#endif

#ifdef DJGPP
  /* Apparently necessary for djgpp 2.01.  May cause problems with      */
  /* other versions.                                                    */
  typedef long unsigned int caddr_t;
#endif

#ifdef PCR
# include "il/PCR_IL.h"
# include "th/PCR_ThCtl.h"
# include "mm/PCR_MM.h"
#endif

#if !defined(NO_EXECUTE_PERMISSION)
  STATIC MK_GC_bool MK_GC_pages_executable = TRUE;
#else
  STATIC MK_GC_bool MK_GC_pages_executable = FALSE;
#endif
#define IGNORE_PAGES_EXECUTABLE 1
                        /* Undefined on MK_GC_pages_executable real use.   */

#ifdef NEED_PROC_MAPS
/* We need to parse /proc/self/maps, either to find dynamic libraries,  */
/* and/or to find the register backing store base (IA64).  Do it once   */
/* here.                                                                */

#define READ read

/* Repeatedly perform a read call until the buffer is filled or */
/* we encounter EOF.                                            */
STATIC ssize_t MK_GC_repeat_read(int fd, char *buf, size_t count)
{
    size_t num_read = 0;
    ssize_t result;

    ASSERT_CANCEL_DISABLED();
    while (num_read < count) {
        result = READ(fd, buf + num_read, count - num_read);
        if (result < 0) return result;
        if (result == 0) break;
        num_read += result;
    }
    return num_read;
}

#ifdef THREADS
  /* Determine the length of a file by incrementally reading it into a  */
  /* This would be silly to use on a file supporting lseek, but Linux   */
  /* /proc files usually do not.                                        */
  STATIC size_t MK_GC_get_file_len(int f)
  {
    size_t total = 0;
    ssize_t result;
#   define GET_FILE_LEN_BUF_SZ 500
    char buf[GET_FILE_LEN_BUF_SZ];

    do {
        result = read(f, buf, GET_FILE_LEN_BUF_SZ);
        if (result == -1) return 0;
        total += result;
    } while (result > 0);
    return total;
  }

  STATIC size_t MK_GC_get_maps_len(void)
  {
    int f = open("/proc/self/maps", O_RDONLY);
    size_t result;
    if (f < 0) return 0; /* treat missing file as empty */
    result = MK_GC_get_file_len(f);
    close(f);
    return result;
  }
#endif /* THREADS */

/* Copy the contents of /proc/self/maps to a buffer in our address      */
/* space.  Return the address of the buffer, or zero on failure.        */
/* This code could be simplified if we could determine its size ahead   */
/* of time.                                                             */
MK_GC_INNER char * MK_GC_get_maps(void)
{
    int f;
    ssize_t result;
    static char *maps_buf = NULL;
    static size_t maps_buf_sz = 1;
    size_t maps_size, old_maps_size = 0;

    /* The buffer is essentially static, so there must be a single client. */
    MK_GC_ASSERT(I_HOLD_LOCK());

    /* Note that in the presence of threads, the maps file can  */
    /* essentially shrink asynchronously and unexpectedly as    */
    /* threads that we already think of as dead release their   */
    /* stacks.  And there is no easy way to read the entire     */
    /* file atomically.  This is arguably a misfeature of the   */
    /* /proc/.../maps interface.                                */

    /* Since we don't believe the file can grow                 */
    /* asynchronously, it should suffice to first determine     */
    /* the size (using lseek or read), and then to reread the   */
    /* file.  If the size is inconsistent we have to retry.     */
    /* This only matters with threads enabled, and if we use    */
    /* this to locate roots (not the default).                  */

#   ifdef THREADS
        /* Determine the initial size of /proc/self/maps.       */
        /* Note that lseek doesn't work, at least as of 2.6.15. */
        maps_size = MK_GC_get_maps_len();
        if (0 == maps_size) return 0;
#   else
        maps_size = 4000;       /* Guess */
#   endif

    /* Read /proc/self/maps, growing maps_buf as necessary.     */
    /* Note that we may not allocate conventionally, and        */
    /* thus can't use stdio.                                    */
        do {
            while (maps_size >= maps_buf_sz) {
              /* Grow only by powers of 2, since we leak "too small" buffers.*/
              while (maps_size >= maps_buf_sz) maps_buf_sz *= 2;
              maps_buf = MK_GC_scratch_alloc(maps_buf_sz);
#             ifdef THREADS
                /* Recompute initial length, since we allocated.        */
                /* This can only happen a few times per program         */
                /* execution.                                           */
                maps_size = MK_GC_get_maps_len();
                if (0 == maps_size) return 0;
#             endif
              if (maps_buf == 0) return 0;
            }
            MK_GC_ASSERT(maps_buf_sz >= maps_size + 1);
            f = open("/proc/self/maps", O_RDONLY);
            if (-1 == f) return 0;
#           ifdef THREADS
              old_maps_size = maps_size;
#           endif
            maps_size = 0;
            do {
                result = MK_GC_repeat_read(f, maps_buf, maps_buf_sz-1);
                if (result <= 0)
                  break;
                maps_size += result;
            } while ((size_t)result == maps_buf_sz-1);
            close(f);
            if (result <= 0)
              return 0;
#           ifdef THREADS
              if (maps_size > old_maps_size) {
                ABORT_ARG2("Unexpected asynchronous /proc/self/maps growth "
                           "(unregistered thread?)", " from %lu to %lu",
                           (unsigned long)old_maps_size,
                           (unsigned long)maps_size);
              }
#           endif
        } while (maps_size >= maps_buf_sz || maps_size < old_maps_size);
                /* In the single-threaded case, the second clause is false. */
        maps_buf[maps_size] = '\0';

        /* Apply fn to result.  */
        return maps_buf;
}

/*
 *  MK_GC_parse_map_entry parses an entry from /proc/self/maps so we can
 *  locate all writable data segments that belong to shared libraries.
 *  The format of one of these entries and the fields we care about
 *  is as follows:
 *  XXXXXXXX-XXXXXXXX r-xp 00000000 30:05 260537     name of mapping...\n
 *  ^^^^^^^^ ^^^^^^^^ ^^^^          ^^
 *  start    end      prot          maj_dev
 *
 *  Note that since about august 2003 kernels, the columns no longer have
 *  fixed offsets on 64-bit kernels.  Hence we no longer rely on fixed offsets
 *  anywhere, which is safer anyway.
 */

/* Assign various fields of the first line in buf_ptr to (*start),      */
/* (*end), (*prot), (*maj_dev) and (*mapping_name).  mapping_name may   */
/* be NULL. (*prot) and (*mapping_name) are assigned pointers into the  */
/* original buffer.                                                     */
#if (defined(DYNAMIC_LOADING) && defined(USE_PROC_FOR_LIBRARIES)) \
    || defined(IA64) || defined(INCLUDE_LINUX_THREAD_DESCR) \
    || defined(REDIRECT_MALLOC)
  MK_GC_INNER char *MK_GC_parse_map_entry(char *buf_ptr, ptr_t *start, ptr_t *end,
                                    char **prot, unsigned int *maj_dev,
                                    char **mapping_name)
  {
    char *start_start, *end_start, *maj_dev_start;
    char *p;
    char *endp;

    if (buf_ptr == NULL || *buf_ptr == '\0') {
        return NULL;
    }

    p = buf_ptr;
    while (isspace(*p)) ++p;
    start_start = p;
    MK_GC_ASSERT(isxdigit(*start_start));
    *start = (ptr_t)strtoul(start_start, &endp, 16); p = endp;
    MK_GC_ASSERT(*p=='-');

    ++p;
    end_start = p;
    MK_GC_ASSERT(isxdigit(*end_start));
    *end = (ptr_t)strtoul(end_start, &endp, 16); p = endp;
    MK_GC_ASSERT(isspace(*p));

    while (isspace(*p)) ++p;
    MK_GC_ASSERT(*p == 'r' || *p == '-');
    *prot = p;
    /* Skip past protection field to offset field */
       while (!isspace(*p)) ++p; while (isspace(*p)) ++p;
    MK_GC_ASSERT(isxdigit(*p));
    /* Skip past offset field, which we ignore */
          while (!isspace(*p)) ++p; while (isspace(*p)) ++p;
    maj_dev_start = p;
    MK_GC_ASSERT(isxdigit(*maj_dev_start));
    *maj_dev = strtoul(maj_dev_start, NULL, 16);

    if (mapping_name == 0) {
      while (*p && *p++ != '\n');
    } else {
      while (*p && *p != '\n' && *p != '/' && *p != '[') p++;
      *mapping_name = p;
      while (*p && *p++ != '\n');
    }
    return p;
  }
#endif /* REDIRECT_MALLOC || DYNAMIC_LOADING || IA64 || ... */

#if defined(IA64) || defined(INCLUDE_LINUX_THREAD_DESCR)
  /* Try to read the backing store base from /proc/self/maps.           */
  /* Return the bounds of the writable mapping with a 0 major device,   */
  /* which includes the address passed as data.                         */
  /* Return FALSE if there is no such mapping.                          */
  MK_GC_INNER MK_GC_bool MK_GC_enclosing_mapping(ptr_t addr, ptr_t *startp,
                                        ptr_t *endp)
  {
    char *prot;
    ptr_t my_start, my_end;
    unsigned int maj_dev;
    char *maps = MK_GC_get_maps();
    char *buf_ptr = maps;

    if (0 == maps) return(FALSE);
    for (;;) {
      buf_ptr = MK_GC_parse_map_entry(buf_ptr, &my_start, &my_end,
                                   &prot, &maj_dev, 0);

      if (buf_ptr == NULL) return FALSE;
      if (prot[1] == 'w' && maj_dev == 0) {
          if ((word)my_end > (word)addr && (word)my_start <= (word)addr) {
            *startp = my_start;
            *endp = my_end;
            return TRUE;
          }
      }
    }
    return FALSE;
  }
#endif /* IA64 || INCLUDE_LINUX_THREAD_DESCR */

#if defined(REDIRECT_MALLOC)
  /* Find the text(code) mapping for the library whose name, after      */
  /* stripping the directory part, starts with nm.                      */
  MK_GC_INNER MK_GC_bool MK_GC_text_mapping(char *nm, ptr_t *startp, ptr_t *endp)
  {
    size_t nm_len = strlen(nm);
    char *prot;
    char *map_path;
    ptr_t my_start, my_end;
    unsigned int maj_dev;
    char *maps = MK_GC_get_maps();
    char *buf_ptr = maps;

    if (0 == maps) return(FALSE);
    for (;;) {
      buf_ptr = MK_GC_parse_map_entry(buf_ptr, &my_start, &my_end,
                                   &prot, &maj_dev, &map_path);

      if (buf_ptr == NULL) return FALSE;
      if (prot[0] == 'r' && prot[1] == '-' && prot[2] == 'x') {
          char *p = map_path;
          /* Set p to point just past last slash, if any. */
            while (*p != '\0' && *p != '\n' && *p != ' ' && *p != '\t') ++p;
            while (*p != '/' && (word)p >= (word)map_path) --p;
            ++p;
          if (strncmp(nm, p, nm_len) == 0) {
            *startp = my_start;
            *endp = my_end;
            return TRUE;
          }
      }
    }
    return FALSE;
  }
#endif /* REDIRECT_MALLOC */

#ifdef IA64
  static ptr_t backing_store_base_from_proc(void)
  {
    ptr_t my_start, my_end;
    if (!MK_GC_enclosing_mapping(MK_GC_save_regs_in_stack(), &my_start, &my_end)) {
        MK_GC_COND_LOG_PRINTF("Failed to find backing store base from /proc\n");
        return 0;
    }
    return my_start;
  }
#endif

#endif /* NEED_PROC_MAPS */

#if defined(SEARCH_FOR_DATA_START)
  /* The I386 case can be handled without a search.  The Alpha case     */
  /* used to be handled differently as well, but the rules changed      */
  /* for recent Linux versions.  This seems to be the easiest way to    */
  /* cover all versions.                                                */

# if defined(LINUX) || defined(HURD)
    /* Some Linux distributions arrange to define __data_start.  Some   */
    /* define data_start as a weak symbol.  The latter is technically   */
    /* broken, since the user program may define data_start, in which   */
    /* case we lose.  Nonetheless, we try both, preferring __data_start.*/
    /* We assume gcc-compatible pragmas.        */
#   pragma weak __data_start
    extern int __data_start[];
#   pragma weak data_start
    extern int data_start[];
# endif /* LINUX */
  extern int _end[];

  ptr_t MK_GC_data_start = NULL;

  ptr_t MK_GC_find_limit(ptr_t, MK_GC_bool);

  MK_GC_INNER void MK_GC_init_linux_data_start(void)
  {
#   if (defined(LINUX) || defined(HURD)) && !defined(IGNORE_PROG_DATA_START)
      /* Try the easy approaches first: */
      if ((ptr_t)__data_start != 0) {
          MK_GC_data_start = (ptr_t)(__data_start);
          MK_GC_ASSERT((word)MK_GC_data_start <= (word)_end);
          return;
      }
      if ((ptr_t)data_start != 0) {
          MK_GC_data_start = (ptr_t)(data_start);
          MK_GC_ASSERT((word)MK_GC_data_start <= (word)_end);
          return;
      }
#     ifdef DEBUG_ADD_DEL_ROOTS
        MK_GC_log_printf("__data_start not provided\n");
#     endif
#   endif /* LINUX */

    if (MK_GC_no_dls) {
      /* Not needed, avoids the SIGSEGV caused by       */
      /* MK_GC_find_limit which complicates debugging.     */
      MK_GC_data_start = (ptr_t)_end; /* set data root size to 0 */
      return;
    }

    MK_GC_data_start = MK_GC_find_limit((ptr_t)(_end), FALSE);
  }
#endif /* SEARCH_FOR_DATA_START */

#ifdef ECOS

# ifndef ECOS_MK_GC_MEMORY_SIZE
#   define ECOS_MK_GC_MEMORY_SIZE (448 * 1024)
# endif /* ECOS_MK_GC_MEMORY_SIZE */

  /* FIXME: This is a simple way of allocating memory which is          */
  /* compatible with ECOS early releases.  Later releases use a more    */
  /* sophisticated means of allocating memory than this simple static   */
  /* allocator, but this method is at least bound to work.              */
  static char ecos_gc_memory[ECOS_MK_GC_MEMORY_SIZE];
  static char *ecos_gc_brk = ecos_gc_memory;

  static void *tiny_sbrk(ptrdiff_t increment)
  {
    void *p = ecos_gc_brk;
    ecos_gc_brk += increment;
    if ((word)ecos_gc_brk > (word)(ecos_gc_memory + sizeof(ecos_gc_memory))) {
      ecos_gc_brk -= increment;
      return NULL;
    }
    return p;
  }
# define sbrk tiny_sbrk
#endif /* ECOS */

#if defined(NETBSD) && defined(__ELF__)
  ptr_t MK_GC_data_start = NULL;
  ptr_t MK_GC_find_limit(ptr_t, MK_GC_bool);

  extern char **environ;

  MK_GC_INNER void MK_GC_init_netbsd_elf(void)
  {
        /* This may need to be environ, without the underscore, for     */
        /* some versions.                                               */
    MK_GC_data_start = MK_GC_find_limit((ptr_t)&environ, FALSE);
  }
#endif /* NETBSD */

#ifdef OPENBSD
  static struct sigaction old_segv_act;
  STATIC sigjmp_buf MK_GC_jmp_buf_openbsd;

# ifdef THREADS
#   include <sys/syscall.h>
    extern sigset_t __syscall(quad_t, ...);
# endif

  /* Don't use MK_GC_find_limit() because siglongjmp() outside of the      */
  /* signal handler by-passes our userland pthreads lib, leaving        */
  /* SIGSEGV and SIGPROF masked.  Instead, use this custom one that     */
  /* works-around the issues.                                           */

  STATIC void MK_GC_fault_handler_openbsd(int sig MK_GC_ATTR_UNUSED)
  {
     siglongjmp(MK_GC_jmp_buf_openbsd, 1);
  }

  /* Return the first non-addressable location > p or bound.    */
  /* Requires the allocation lock.                              */
  STATIC ptr_t MK_GC_find_limit_openbsd(ptr_t p, ptr_t bound)
  {
    static volatile ptr_t result;
             /* Safer if static, since otherwise it may not be  */
             /* preserved across the longjmp.  Can safely be    */
             /* static since it's only called with the          */
             /* allocation lock held.                           */

    struct sigaction act;
    size_t pgsz = (size_t)sysconf(_SC_PAGESIZE);
    MK_GC_ASSERT(I_HOLD_LOCK());

    act.sa_handler = MK_GC_fault_handler_openbsd;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESTART;
    /* act.sa_restorer is deprecated and should not be initialized. */
    sigaction(SIGSEGV, &act, &old_segv_act);

    if (sigsetjmp(MK_GC_jmp_buf_openbsd, 1) == 0) {
      result = (ptr_t)((word)p & ~(pgsz-1));
      for (;;) {
        result += pgsz;
        if ((word)result >= (word)bound) {
          result = bound;
          break;
        }
        MK_GC_noop1((word)(*result));
      }
    }

#   ifdef THREADS
      /* Due to the siglongjump we need to manually unmask SIGPROF.     */
      __syscall(SYS_sigprocmask, SIG_UNBLOCK, sigmask(SIGPROF));
#   endif

    sigaction(SIGSEGV, &old_segv_act, 0);
    return(result);
  }

  /* Return first addressable location > p or bound.    */
  /* Requires the allocation lock.                      */
  STATIC ptr_t MK_GC_skip_hole_openbsd(ptr_t p, ptr_t bound)
  {
    static volatile ptr_t result;
    static volatile int firstpass;

    struct sigaction act;
    size_t pgsz = (size_t)sysconf(_SC_PAGESIZE);
    MK_GC_ASSERT(I_HOLD_LOCK());

    act.sa_handler = MK_GC_fault_handler_openbsd;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESTART;
    /* act.sa_restorer is deprecated and should not be initialized. */
    sigaction(SIGSEGV, &act, &old_segv_act);

    firstpass = 1;
    result = (ptr_t)((word)p & ~(pgsz-1));
    if (sigsetjmp(MK_GC_jmp_buf_openbsd, 1) != 0 || firstpass) {
      firstpass = 0;
      result += pgsz;
      if ((word)result >= (word)bound) {
        result = bound;
      } else {
        MK_GC_noop1((word)(*result));
      }
    }

    sigaction(SIGSEGV, &old_segv_act, 0);
    return(result);
  }
#endif /* OPENBSD */

# ifdef OS2

# include <stddef.h>

# if !defined(__IBMC__) && !defined(__WATCOMC__) /* e.g. EMX */

struct exe_hdr {
    unsigned short      magic_number;
    unsigned short      padding[29];
    long                new_exe_offset;
};

#define E_MAGIC(x)      (x).magic_number
#define EMAGIC          0x5A4D
#define E_LFANEW(x)     (x).new_exe_offset

struct e32_exe {
    unsigned char       magic_number[2];
    unsigned char       byte_order;
    unsigned char       word_order;
    unsigned long       exe_format_level;
    unsigned short      cpu;
    unsigned short      os;
    unsigned long       padding1[13];
    unsigned long       object_table_offset;
    unsigned long       object_count;
    unsigned long       padding2[31];
};

#define E32_MAGIC1(x)   (x).magic_number[0]
#define E32MAGIC1       'L'
#define E32_MAGIC2(x)   (x).magic_number[1]
#define E32MAGIC2       'X'
#define E32_BORDER(x)   (x).byte_order
#define E32LEBO         0
#define E32_WORDER(x)   (x).word_order
#define E32LEWO         0
#define E32_CPU(x)      (x).cpu
#define E32CPU286       1
#define E32_OBJTAB(x)   (x).object_table_offset
#define E32_OBJCNT(x)   (x).object_count

struct o32_obj {
    unsigned long       size;
    unsigned long       base;
    unsigned long       flags;
    unsigned long       pagemap;
    unsigned long       mapsize;
    unsigned long       reserved;
};

#define O32_FLAGS(x)    (x).flags
#define OBJREAD         0x0001L
#define OBJWRITE        0x0002L
#define OBJINVALID      0x0080L
#define O32_SIZE(x)     (x).size
#define O32_BASE(x)     (x).base

# else  /* IBM's compiler */

/* A kludge to get around what appears to be a header file bug */
# ifndef WORD
#   define WORD unsigned short
# endif
# ifndef DWORD
#   define DWORD unsigned long
# endif

# define EXE386 1
# include <newexe.h>
# include <exe386.h>

# endif  /* __IBMC__ */

# define INCL_DOSEXCEPTIONS
# define INCL_DOSPROCESS
# define INCL_DOSERRORS
# define INCL_DOSMODULEMGR
# define INCL_DOSMEMMGR
# include <os2.h>

# endif /* OS/2 */

/* Find the page size */
MK_GC_INNER word MK_GC_page_size = 0;

#if defined(MSWIN32) || defined(MSWINCE) || defined(CYGWIN32)
# ifndef VER_PLATFORM_WIN32_CE
#   define VER_PLATFORM_WIN32_CE 3
# endif

# if defined(MSWINCE) && defined(THREADS)
    MK_GC_INNER MK_GC_bool MK_GC_dont_query_stack_min = FALSE;
# endif

  MK_GC_INNER SYSTEM_INFO MK_GC_sysinfo;

  MK_GC_INNER void MK_GC_setpagesize(void)
  {
    GetSystemInfo(&MK_GC_sysinfo);
    MK_GC_page_size = MK_GC_sysinfo.dwPageSize;
#   if defined(MSWINCE) && !defined(_WIN32_WCE_EMULATION)
      {
        OSVERSIONINFO verInfo;
        /* Check the current WinCE version.     */
        verInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
        if (!GetVersionEx(&verInfo))
          ABORT("GetVersionEx failed");
        if (verInfo.dwPlatformId == VER_PLATFORM_WIN32_CE &&
            verInfo.dwMajorVersion < 6) {
          /* Only the first 32 MB of address space belongs to the       */
          /* current process (unless WinCE 6.0+ or emulation).          */
          MK_GC_sysinfo.lpMaximumApplicationAddress = (LPVOID)((word)32 << 20);
#         ifdef THREADS
            /* On some old WinCE versions, it's observed that           */
            /* VirtualQuery calls don't work properly when used to      */
            /* get thread current stack committed minimum.              */
            if (verInfo.dwMajorVersion < 5)
              MK_GC_dont_query_stack_min = TRUE;
#         endif
        }
      }
#   endif
  }

# ifndef CYGWIN32
#   define is_writable(prot) ((prot) == PAGE_READWRITE \
                            || (prot) == PAGE_WRITECOPY \
                            || (prot) == PAGE_EXECUTE_READWRITE \
                            || (prot) == PAGE_EXECUTE_WRITECOPY)
#  if 0  /* "#if 0" for MKCL 1.1.3 and later, "#if 1" before. JCB */
    /* Return the number of bytes that are writable starting at p.      */
    /* The pointer p is assumed to be page aligned.                     */
    /* If base is not 0, *base becomes the beginning of the             */
    /* allocation region containing p.                                  */
    STATIC word MK_GC_get_writable_length(ptr_t p, ptr_t *base)
    {
      MEMORY_BASIC_INFORMATION buf;
      word result;
      word protect;

      result = VirtualQuery(p, &buf, sizeof(buf));
      if (result != sizeof(buf)) ABORT("Weird VirtualQuery result");
      if (base != 0) *base = (ptr_t)(buf.AllocationBase);
      protect = (buf.Protect & ~(PAGE_GUARD | PAGE_NOCACHE));
      if (!is_writable(protect)) {
        return(0);
      }
      if (buf.State != MEM_COMMIT) return(0);
      return(buf.RegionSize);
    }

    MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
    {
      ptr_t trunc_sp = (ptr_t)((word)MK_GC_approx_sp() & ~(MK_GC_page_size - 1));
      /* FIXME: This won't work if called from a deeply recursive       */
      /* client code (and the committed stack space has grown).         */
      word size = MK_GC_get_writable_length(trunc_sp, 0);
      MK_GC_ASSERT(size != 0);
      sb -> mem_base = trunc_sp + size;
      return MK_GC_SUCCESS;
    }
#  else
    MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
    { /* JCB */
      word a_var = 0;
      MEMORY_BASIC_INFORMATION mbi;

      SIZE_T mbi_size = VirtualQuery(&a_var, &mbi, sizeof(mbi));

      if (mbi_size != sizeof(mbi)) ABORT("Weird VirtualQuery result");

      ptr_t stack_top = mbi.BaseAddress + mbi.RegionSize;
      ptr_t stack_base = mbi.AllocationBase;
      word stack_size = stack_top - stack_base;

      if (stack_size == 0) ABORT("VirtualQuery reported a stack of size 0");

      sb -> mem_base = stack_top;
      return MK_GC_SUCCESS;
    }
#  endif
# else /* CYGWIN32 */
    /* An alternate version for Cygwin (adapted from Dave Korn's        */
    /* gcc version of boehm-gc).                                        */
    MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
    {
      void * _tlsbase;

      __asm__ ("movl %%fs:4, %0"
               : "=r" (_tlsbase));
      sb -> mem_base = _tlsbase;
      return MK_GC_SUCCESS;
    }
# endif /* CYGWIN32 */
# define HAVE_GET_STACK_BASE

#else /* !MSWIN32 */
  MK_GC_INNER void MK_GC_setpagesize(void)
  {
#   if defined(MPROTECT_VDB) || defined(PROC_VDB) || defined(USE_MMAP)
      MK_GC_page_size = GETPAGESIZE();
      if (!MK_GC_page_size) ABORT("getpagesize failed");
#   else
      /* It's acceptable to fake it.    */
      MK_GC_page_size = HBLKSIZE;
#   endif
  }
#endif /* !MSWIN32 */

#ifdef BEOS
# include <kernel/OS.h>

  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
  {
    thread_info th;
    get_thread_info(find_thread(NULL),&th);
    sb->mem_base = th.stack_end;
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* BEOS */

#ifdef OS2
  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
  {
    PTIB ptib; /* thread information block */
    PPIB ppib;
    if (DosGetInfoBlocks(&ptib, &ppib) != NO_ERROR) {
      ABORT("DosGetInfoBlocks failed");
    }
    sb->mem_base = ptib->tib_pstacklimit;
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* OS2 */

# ifdef AMIGA
#   define MK_GC_AMIGA_SB
#   include "extra/AmigaOS.c"
#   undef MK_GC_AMIGA_SB
# endif /* AMIGA */

# if defined(NEED_FIND_LIMIT) || defined(UNIX_LIKE)

    typedef void (*MK_GC_fault_handler_t)(int);

#   if defined(SUNOS5SIGS) || defined(IRIX5) || defined(OSF1) \
       || defined(HURD) || defined(FREEBSD) || defined(NETBSD)
        static struct sigaction old_segv_act;
#       if defined(_sigargs) /* !Irix6.x */ \
           || defined(HURD) || defined(NETBSD) || defined(FREEBSD)
            static struct sigaction old_bus_act;
#       endif
#   else
      static MK_GC_fault_handler_t old_segv_handler;
#     ifdef SIGBUS
        static MK_GC_fault_handler_t old_bus_handler;
#     endif
#   endif

    MK_GC_INNER void MK_GC_set_and_save_fault_handler(MK_GC_fault_handler_t h)
    {
#       if defined(SUNOS5SIGS) || defined(IRIX5) || defined(OSF1) \
            || defined(HURD) || defined(FREEBSD) || defined(NETBSD)
          struct sigaction act;

          act.sa_handler = h;
#         ifdef SIGACTION_FLAGS_NODEFER_HACK
            /* Was necessary for Solaris 2.3 and very temporary */
            /* NetBSD bugs.                                     */
            act.sa_flags = SA_RESTART | SA_NODEFER;
#         else
            act.sa_flags = SA_RESTART;
#         endif

          (void) sigemptyset(&act.sa_mask);
          /* act.sa_restorer is deprecated and should not be initialized. */
#         ifdef MK_GC_IRIX_THREADS
            /* Older versions have a bug related to retrieving and      */
            /* and setting a handler at the same time.                  */
            (void) sigaction(SIGSEGV, 0, &old_segv_act);
            (void) sigaction(SIGSEGV, &act, 0);
#         else
            (void) sigaction(SIGSEGV, &act, &old_segv_act);
#           if defined(IRIX5) && defined(_sigargs) /* Irix 5.x, not 6.x */ \
               || defined(HURD) || defined(NETBSD) || defined(FREEBSD)
              /* Under Irix 5.x or HP/UX, we may get SIGBUS.    */
              /* Pthreads doesn't exist under Irix 5.x, so we   */
              /* don't have to worry in the threads case.       */
              (void) sigaction(SIGBUS, &act, &old_bus_act);
#           endif
#         endif /* !MK_GC_IRIX_THREADS */
#       else
          old_segv_handler = signal(SIGSEGV, h);
#         ifdef SIGBUS
            old_bus_handler = signal(SIGBUS, h);
#         endif
#       endif
    }
# endif /* NEED_FIND_LIMIT || UNIX_LIKE */

# if defined(NEED_FIND_LIMIT) \
     || (defined(USE_PROC_FOR_LIBRARIES) && defined(THREADS))
  /* Some tools to implement HEURISTIC2 */
#   define MIN_PAGE_SIZE 256    /* Smallest conceivable page size, bytes */

    STATIC void MK_GC_fault_handler(int sig MK_GC_ATTR_UNUSED)
    {
        LONGJMP(MK_GC_jmp_buf, 1);
    }

    MK_GC_INNER void MK_GC_setup_temporary_fault_handler(void)
    {
        /* Handler is process-wide, so this should only happen in       */
        /* one thread at a time.                                        */
        MK_GC_ASSERT(I_HOLD_LOCK());
        MK_GC_set_and_save_fault_handler(MK_GC_fault_handler);
    }

    MK_GC_INNER void MK_GC_reset_fault_handler(void)
    {
#       if defined(SUNOS5SIGS) || defined(IRIX5) || defined(OSF1) \
           || defined(HURD) || defined(FREEBSD) || defined(NETBSD)
          (void) sigaction(SIGSEGV, &old_segv_act, 0);
#         if defined(IRIX5) && defined(_sigargs) /* Irix 5.x, not 6.x */ \
             || defined(HURD) || defined(NETBSD)
              (void) sigaction(SIGBUS, &old_bus_act, 0);
#         endif
#       else
          (void) signal(SIGSEGV, old_segv_handler);
#         ifdef SIGBUS
            (void) signal(SIGBUS, old_bus_handler);
#         endif
#       endif
    }

    /* Return the first non-addressable location > p (up) or    */
    /* the smallest location q s.t. [q,p) is addressable (!up). */
    /* We assume that p (up) or p-1 (!up) is addressable.       */
    /* Requires allocation lock.                                */
    STATIC ptr_t MK_GC_find_limit_with_bound(ptr_t p, MK_GC_bool up, ptr_t bound)
    {
        static volatile ptr_t result;
                /* Safer if static, since otherwise it may not be       */
                /* preserved across the longjmp.  Can safely be         */
                /* static since it's only called with the               */
                /* allocation lock held.                                */

        MK_GC_ASSERT(I_HOLD_LOCK());
        MK_GC_setup_temporary_fault_handler();
        if (SETJMP(MK_GC_jmp_buf) == 0) {
            result = (ptr_t)(((word)(p))
                              & ~(MIN_PAGE_SIZE-1));
            for (;;) {
                if (up) {
                    result += MIN_PAGE_SIZE;
                    if ((word)result >= (word)bound) {
                      result = bound;
                      break;
                    }
                } else {
                    result -= MIN_PAGE_SIZE;
                    if ((word)result <= (word)bound) {
                      result = bound - MIN_PAGE_SIZE;
                                        /* This is to compensate        */
                                        /* further result increment (we */
                                        /* do not modify "up" variable  */
                                        /* since it might be clobbered  */
                                        /* by setjmp otherwise).        */
                      break;
                    }
                }
                MK_GC_noop1((word)(*result));
            }
        }
        MK_GC_reset_fault_handler();
        if (!up) {
            result += MIN_PAGE_SIZE;
        }
        return(result);
    }

    ptr_t MK_GC_find_limit(ptr_t p, MK_GC_bool up)
    {
        return MK_GC_find_limit_with_bound(p, up, up ? (ptr_t)(word)(-1) : 0);
    }
# endif /* NEED_FIND_LIMIT || USE_PROC_FOR_LIBRARIES */

#ifdef HPUX_STACKBOTTOM

#include <sys/param.h>
#include <sys/pstat.h>

  MK_GC_INNER ptr_t MK_GC_get_register_stack_base(void)
  {
    struct pst_vm_status vm_status;

    int i = 0;
    while (pstat_getprocvm(&vm_status, sizeof(vm_status), 0, i++) == 1) {
      if (vm_status.pst_type == PS_RSESTACK) {
        return (ptr_t) vm_status.pst_vaddr;
      }
    }

    /* old way to get the register stackbottom */
    return (ptr_t)(((word)MK_GC_stackbottom - BACKING_STORE_DISPLACEMENT - 1)
                   & ~(BACKING_STORE_ALIGNMENT - 1));
  }

#endif /* HPUX_STACK_BOTTOM */

#ifdef LINUX_STACKBOTTOM

# include <sys/types.h>
# include <sys/stat.h>

# define STAT_SKIP 27   /* Number of fields preceding startstack        */
                        /* field in /proc/self/stat                     */

# ifdef USE_LIBC_PRIVATES
#   pragma weak __libc_stack_end
    extern ptr_t __libc_stack_end;
# endif

# ifdef IA64
#   ifdef USE_LIBC_PRIVATES
#     pragma weak __libc_ia64_register_backing_store_base
      extern ptr_t __libc_ia64_register_backing_store_base;
#   endif

    MK_GC_INNER ptr_t MK_GC_get_register_stack_base(void)
    {
      ptr_t result;

#     ifdef USE_LIBC_PRIVATES
        if (0 != &__libc_ia64_register_backing_store_base
            && 0 != __libc_ia64_register_backing_store_base) {
          /* Glibc 2.2.4 has a bug such that for dynamically linked     */
          /* executables __libc_ia64_register_backing_store_base is     */
          /* defined but uninitialized during constructor calls.        */
          /* Hence we check for both nonzero address and value.         */
          return __libc_ia64_register_backing_store_base;
        }
#     endif
      result = backing_store_base_from_proc();
      if (0 == result) {
          result = MK_GC_find_limit(MK_GC_save_regs_in_stack(), FALSE);
          /* Now seems to work better than constant displacement        */
          /* heuristic used in 6.X versions.  The latter seems to       */
          /* fail for 2.6 kernels.                                      */
      }
      return result;
    }
# endif /* IA64 */

  STATIC ptr_t MK_GC_linux_main_stack_base(void)
  {
    /* We read the stack base value from /proc/self/stat.  We do this   */
    /* using direct I/O system calls in order to avoid calling malloc   */
    /* in case REDIRECT_MALLOC is defined.                              */
#   ifndef STAT_READ
      /* Also defined in pthread_support.c. */
#     define STAT_BUF_SIZE 4096
#     define STAT_READ read
#   endif
          /* Should probably call the real read, if read is wrapped.    */
    char stat_buf[STAT_BUF_SIZE];
    int f;
    word result;
    int i, buf_offset = 0, len;

    /* First try the easy way.  This should work for glibc 2.2  */
    /* This fails in a prelinked ("prelink" command) executable */
    /* since the correct value of __libc_stack_end never        */
    /* becomes visible to us.  The second test works around     */
    /* this.                                                    */
#   ifdef USE_LIBC_PRIVATES
      if (0 != &__libc_stack_end && 0 != __libc_stack_end ) {
#       if defined(IA64)
          /* Some versions of glibc set the address 16 bytes too        */
          /* low while the initialization code is running.              */
          if (((word)__libc_stack_end & 0xfff) + 0x10 < 0x1000) {
            return __libc_stack_end + 0x10;
          } /* Otherwise it's not safe to add 16 bytes and we fall      */
            /* back to using /proc.                                     */
#       elif defined(SPARC)
          /* Older versions of glibc for 64-bit SPARC do not set this   */
          /* variable correctly, it gets set to either zero or one.     */
          if (__libc_stack_end != (ptr_t) (unsigned long)0x1)
            return __libc_stack_end;
#       else
          return __libc_stack_end;
#       endif
      }
#   endif
    f = open("/proc/self/stat", O_RDONLY);
    if (f < 0)
      ABORT("Couldn't read /proc/self/stat");
    len = STAT_READ(f, stat_buf, STAT_BUF_SIZE);
    close(f);

    /* Skip the required number of fields.  This number is hopefully    */
    /* constant across all Linux implementations.                       */
    for (i = 0; i < STAT_SKIP; ++i) {
      while (buf_offset < len && isspace(stat_buf[buf_offset++])) {
        /* empty */
      }
      while (buf_offset < len && !isspace(stat_buf[buf_offset++])) {
        /* empty */
      }
    }
    /* Skip spaces.     */
    while (buf_offset < len && isspace(stat_buf[buf_offset])) {
      buf_offset++;
    }
    /* Find the end of the number and cut the buffer there.     */
    for (i = 0; buf_offset + i < len; i++) {
      if (!isdigit(stat_buf[buf_offset + i])) break;
    }
    if (buf_offset + i >= len) ABORT("Could not parse /proc/self/stat");
    stat_buf[buf_offset + i] = '\0';

    result = (word)STRTOULL(&stat_buf[buf_offset], NULL, 10);
    if (result < 0x100000 || (result & (sizeof(word) - 1)) != 0)
      ABORT("Absurd stack bottom value");
    return (ptr_t)result;
  }
#endif /* LINUX_STACKBOTTOM */

#ifdef FREEBSD_STACKBOTTOM
  /* This uses an undocumented sysctl call, but at least one expert     */
  /* believes it will stay.                                             */

# include <unistd.h>
# include <sys/types.h>
# include <sys/sysctl.h>

  STATIC ptr_t MK_GC_freebsd_main_stack_base(void)
  {
    int nm[2] = {CTL_KERN, KERN_USRSTACK};
    ptr_t base;
    size_t len = sizeof(ptr_t);
    int r = sysctl(nm, 2, &base, &len, NULL, 0);
    if (r) ABORT("Error getting main stack base");
    return base;
  }
#endif /* FREEBSD_STACKBOTTOM */

#if defined(ECOS) || defined(NOSYS)
  ptr_t MK_GC_get_main_stack_base(void)
  {
    return STACKBOTTOM;
  }
# define GET_MAIN_STACKBASE_SPECIAL
#elif defined(SYMBIAN)
  extern int MK_GC_get_main_symbian_stack_base(void);
  ptr_t MK_GC_get_main_stack_base(void)
  {
    return (ptr_t)MK_GC_get_main_symbian_stack_base();
  }
# define GET_MAIN_STACKBASE_SPECIAL
#elif !defined(BEOS) && !defined(AMIGA) && !defined(OS2) \
      && !defined(MSWIN32) && !defined(MSWINCE) && !defined(CYGWIN32) \
      && !defined(MK_GC_OPENBSD_THREADS) \
      && (!defined(MK_GC_SOLARIS_THREADS) || defined(_STRICT_STDC))

# if defined(LINUX) && defined(USE_GET_STACKBASE_FOR_MAIN)
#   include <pthread.h>
# elif defined(DARWIN) && !defined(NO_PTHREAD_GET_STACKADDR_NP)
    /* We could use pthread_get_stackaddr_np even in case of a  */
    /* single-threaded gclib (there is no -lpthread on Darwin). */
#   include <pthread.h>
#   undef STACKBOTTOM
#   define STACKBOTTOM (ptr_t)pthread_get_stackaddr_np(pthread_self())
# endif

  ptr_t MK_GC_get_main_stack_base(void)
  {
    ptr_t result;
#   if defined(LINUX) && !defined(NO_PTHREAD_GETATTR_NP) \
       && (defined(USE_GET_STACKBASE_FOR_MAIN) \
           || (defined(THREADS) && !defined(REDIRECT_MALLOC)))
      pthread_attr_t attr;
      void *stackaddr;
      size_t size;

      if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        if (pthread_attr_getstack(&attr, &stackaddr, &size) == 0
            && stackaddr != NULL) {
          pthread_attr_destroy(&attr);
#         ifdef STACK_GROWS_DOWN
            stackaddr = (char *)stackaddr + size;
#         endif
          return (ptr_t)stackaddr;
        }
        pthread_attr_destroy(&attr);
      }
      WARN("pthread_getattr_np or pthread_attr_getstack failed"
           " for main thread\n", 0);
#   endif
#   ifdef STACKBOTTOM
      result = STACKBOTTOM;
#   else
#     define STACKBOTTOM_ALIGNMENT_M1 ((word)STACK_GRAN - 1)
#     ifdef HEURISTIC1
#       ifdef STACK_GROWS_DOWN
          result = (ptr_t)(((word)MK_GC_approx_sp() + STACKBOTTOM_ALIGNMENT_M1)
                           & ~STACKBOTTOM_ALIGNMENT_M1);
#       else
          result = (ptr_t)((word)MK_GC_approx_sp() & ~STACKBOTTOM_ALIGNMENT_M1);
#       endif
#     endif /* HEURISTIC1 */
#     ifdef LINUX_STACKBOTTOM
         result = MK_GC_linux_main_stack_base();
#     endif
#     ifdef FREEBSD_STACKBOTTOM
         result = MK_GC_freebsd_main_stack_base();
#     endif
#     ifdef HEURISTIC2
        {
          ptr_t sp = MK_GC_approx_sp();
#         ifdef STACK_GROWS_DOWN
            result = MK_GC_find_limit(sp, TRUE);
#           ifdef HEURISTIC2_LIMIT
              if ((word)result > (word)HEURISTIC2_LIMIT
                  && (word)sp < (word)HEURISTIC2_LIMIT) {
                result = HEURISTIC2_LIMIT;
              }
#           endif
#         else
            result = MK_GC_find_limit(sp, FALSE);
#           ifdef HEURISTIC2_LIMIT
              if ((word)result < (word)HEURISTIC2_LIMIT
                  && (word)sp > (word)HEURISTIC2_LIMIT) {
                result = HEURISTIC2_LIMIT;
              }
#           endif
#         endif
        }
#     endif /* HEURISTIC2 */
#     ifdef STACK_GROWS_DOWN
        if (result == 0)
          result = (ptr_t)(signed_word)(-sizeof(ptr_t));
#     endif
#   endif
    MK_GC_ASSERT((word)MK_GC_approx_sp() HOTTER_THAN (word)result);
    return(result);
  }
# define GET_MAIN_STACKBASE_SPECIAL
#endif /* !AMIGA, !BEOS, !OPENBSD, !OS2, !Windows */

#if (defined(MK_GC_LINUX_THREADS) || defined(PLATFORM_ANDROID)) \
    && !defined(NO_PTHREAD_GETATTR_NP)

# include <pthread.h>
  /* extern int pthread_getattr_np(pthread_t, pthread_attr_t *); */

  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *b)
  {
    pthread_attr_t attr;
    size_t size;
#   ifdef IA64
      DCL_LOCK_STATE;
#   endif

    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        WARN("pthread_getattr_np failed\n", 0);
        return MK_GC_UNIMPLEMENTED;
    }
    if (pthread_attr_getstack(&attr, &(b -> mem_base), &size) != 0) {
        ABORT("pthread_attr_getstack failed");
    }
    pthread_attr_destroy(&attr);
#   ifdef STACK_GROWS_DOWN
        b -> mem_base = (char *)(b -> mem_base) + size;
#   endif
#   ifdef IA64
      /* We could try backing_store_base_from_proc, but that's safe     */
      /* only if no mappings are being asynchronously created.          */
      /* Subtracting the size from the stack base doesn't work for at   */
      /* least the main thread.                                         */
      LOCK();
      {
        IF_CANCEL(int cancel_state;)
        ptr_t bsp;
        ptr_t next_stack;

        DISABLE_CANCEL(cancel_state);
        bsp = MK_GC_save_regs_in_stack();
        next_stack = MK_GC_greatest_stack_base_below(bsp);
        if (0 == next_stack) {
          b -> reg_base = MK_GC_find_limit(bsp, FALSE);
        } else {
          /* Avoid walking backwards into preceding memory stack and    */
          /* growing it.                                                */
          b -> reg_base = MK_GC_find_limit_with_bound(bsp, FALSE, next_stack);
        }
        RESTORE_CANCEL(cancel_state);
      }
      UNLOCK();
#   endif
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* MK_GC_LINUX_THREADS */

#if defined(MK_GC_DARWIN_THREADS) && !defined(NO_PTHREAD_GET_STACKADDR_NP)
# include <pthread.h>

  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *b)
  {
    /* pthread_get_stackaddr_np() should return stack bottom (highest   */
    /* stack address plus 1).                                           */
    b->mem_base = pthread_get_stackaddr_np(pthread_self());
    MK_GC_ASSERT((word)MK_GC_approx_sp() HOTTER_THAN (word)b->mem_base);
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* MK_GC_DARWIN_THREADS */

#ifdef MK_GC_OPENBSD_THREADS
# include <sys/signal.h>
# include <pthread.h>
# include <pthread_np.h>

  /* Find the stack using pthread_stackseg_np(). */
  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
  {
    stack_t stack;
    if (pthread_stackseg_np(pthread_self(), &stack))
      ABORT("pthread_stackseg_np(self) failed");
    sb->mem_base = stack.ss_sp;
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* MK_GC_OPENBSD_THREADS */

#if defined(MK_GC_SOLARIS_THREADS) && !defined(_STRICT_STDC)

# include <thread.h>
# include <signal.h>
# include <pthread.h>

  /* These variables are used to cache ss_sp value for the primordial   */
  /* thread (it's better not to call thr_stksegment() twice for this    */
  /* thread - see JDK bug #4352906).                                    */
  static pthread_t stackbase_main_self = 0;
                        /* 0 means stackbase_main_ss_sp value is unset. */
  static void *stackbase_main_ss_sp = NULL;

  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *b)
  {
    stack_t s;
    pthread_t self = pthread_self();

    if (self == stackbase_main_self)
      {
        /* If the client calls MK_GC_get_stack_base() from the main thread */
        /* then just return the cached value.                           */
        b -> mem_base = stackbase_main_ss_sp;
        MK_GC_ASSERT(b -> mem_base != NULL);
        return MK_GC_SUCCESS;
      }

    if (thr_stksegment(&s)) {
      /* According to the manual, the only failure error code returned  */
      /* is EAGAIN meaning "the information is not available due to the */
      /* thread is not yet completely initialized or it is an internal  */
      /* thread" - this shouldn't happen here.                          */
      ABORT("thr_stksegment failed");
    }
    /* s.ss_sp holds the pointer to the stack bottom. */
    MK_GC_ASSERT((word)MK_GC_approx_sp() HOTTER_THAN (word)s.ss_sp);

    if (!stackbase_main_self && thr_main() != 0)
      {
        /* Cache the stack base value for the primordial thread (this   */
        /* is done during MK_GC_init, so there is no race).                */
        stackbase_main_ss_sp = s.ss_sp;
        stackbase_main_self = self;
      }

    b -> mem_base = s.ss_sp;
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* MK_GC_SOLARIS_THREADS */

#ifdef MK_GC_RTEMS_PTHREADS
  MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *sb)
  {
    sb->mem_base = rtems_get_stack_bottom();
    return MK_GC_SUCCESS;
  }
# define HAVE_GET_STACK_BASE
#endif /* MK_GC_RTEMS_PTHREADS */

#ifndef HAVE_GET_STACK_BASE
# ifdef NEED_FIND_LIMIT
    /* Retrieve stack base.                                             */
    /* Using the MK_GC_find_limit version is risky.                        */
    /* On IA64, for example, there is no guard page between the         */
    /* stack of one thread and the register backing store of the        */
    /* next.  Thus this is likely to identify way too large a           */
    /* "stack" and thus at least result in disastrous performance.      */
    /* FIXME - Implement better strategies here.                        */
    MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(struct MK_GC_stack_base *b)
    {
      IF_CANCEL(int cancel_state;)
      DCL_LOCK_STATE;

      LOCK();
      DISABLE_CANCEL(cancel_state);  /* May be unnecessary? */
#     ifdef STACK_GROWS_DOWN
        b -> mem_base = MK_GC_find_limit(MK_GC_approx_sp(), TRUE);
#       ifdef IA64
          b -> reg_base = MK_GC_find_limit(MK_GC_save_regs_in_stack(), FALSE);
#       endif
#     else
        b -> mem_base = MK_GC_find_limit(MK_GC_approx_sp(), FALSE);
#     endif
      RESTORE_CANCEL(cancel_state);
      UNLOCK();
      return MK_GC_SUCCESS;
    }
# else
    MK_GC_API int MK_GC_CALL MK_GC_get_stack_base(
                                struct MK_GC_stack_base *b MK_GC_ATTR_UNUSED)
    {
#     if defined(GET_MAIN_STACKBASE_SPECIAL) && !defined(THREADS) \
         && !defined(IA64)
        b->mem_base = MK_GC_get_main_stack_base();
        return MK_GC_SUCCESS;
#     else
        return MK_GC_UNIMPLEMENTED;
#     endif
    }
# endif /* !NEED_FIND_LIMIT */
#endif /* !HAVE_GET_STACK_BASE */

#ifndef GET_MAIN_STACKBASE_SPECIAL
  /* This is always called from the main thread.  Default implementation. */
  ptr_t MK_GC_get_main_stack_base(void)
  {
    struct MK_GC_stack_base sb;

    if (MK_GC_get_stack_base(&sb) != MK_GC_SUCCESS)
      ABORT("MK_GC_get_stack_base failed");
    MK_GC_ASSERT((word)MK_GC_approx_sp() HOTTER_THAN (word)sb.mem_base);
    return (ptr_t)sb.mem_base;
  }
#endif /* !GET_MAIN_STACKBASE_SPECIAL */

/* Register static data segment(s) as roots.  If more data segments are */
/* added later then they need to be registered at that point (as we do  */
/* with SunOS dynamic loading), or MK_GC_mark_roots needs to check for     */
/* them (as we do with PCR).  Called with allocator lock held.          */
# ifdef OS2

void MK_GC_register_data_segments(void)
{
    PTIB ptib;
    PPIB ppib;
    HMODULE module_handle;
#   define PBUFSIZ 512
    UCHAR path[PBUFSIZ];
    FILE * myexefile;
    struct exe_hdr hdrdos;      /* MSDOS header.        */
    struct e32_exe hdr386;      /* Real header for my executable */
    struct o32_obj seg;         /* Current segment */
    int nsegs;

    if (DosGetInfoBlocks(&ptib, &ppib) != NO_ERROR) {
        ABORT("DosGetInfoBlocks failed");
    }
    module_handle = ppib -> pib_hmte;
    if (DosQueryModuleName(module_handle, PBUFSIZ, path) != NO_ERROR) {
        ABORT("DosQueryModuleName failed");
    }
    myexefile = fopen(path, "rb");
    if (myexefile == 0) {
        ABORT_ARG1("Failed to open executable", ": %s", path);
    }
    if (fread((char *)(&hdrdos), 1, sizeof(hdrdos), myexefile)
          < sizeof(hdrdos)) {
        ABORT_ARG1("Could not read MSDOS header", " from: %s", path);
    }
    if (E_MAGIC(hdrdos) != EMAGIC) {
        ABORT_ARG1("Bad DOS magic number", " in file: %s", path);
    }
    if (fseek(myexefile, E_LFANEW(hdrdos), SEEK_SET) != 0) {
        ABORT_ARG1("Bad DOS magic number", " in file: %s", path);
    }
    if (fread((char *)(&hdr386), 1, sizeof(hdr386), myexefile)
          < sizeof(hdr386)) {
        ABORT_ARG1("Could not read OS/2 header", " from: %s", path);
    }
    if (E32_MAGIC1(hdr386) != E32MAGIC1 || E32_MAGIC2(hdr386) != E32MAGIC2) {
        ABORT_ARG1("Bad OS/2 magic number", " in file: %s", path);
    }
    if (E32_BORDER(hdr386) != E32LEBO || E32_WORDER(hdr386) != E32LEWO) {
        ABORT_ARG1("Bad byte order in executable", " file: %s", path);
    }
    if (E32_CPU(hdr386) == E32CPU286) {
        ABORT_ARG1("GC cannot handle 80286 executables", ": %s", path);
    }
    if (fseek(myexefile, E_LFANEW(hdrdos) + E32_OBJTAB(hdr386),
              SEEK_SET) != 0) {
        ABORT_ARG1("Seek to object table failed", " in file: %s", path);
    }
    for (nsegs = E32_OBJCNT(hdr386); nsegs > 0; nsegs--) {
      int flags;
      if (fread((char *)(&seg), 1, sizeof(seg), myexefile) < sizeof(seg)) {
        ABORT_ARG1("Could not read obj table entry", " from file: %s", path);
      }
      flags = O32_FLAGS(seg);
      if (!(flags & OBJWRITE)) continue;
      if (!(flags & OBJREAD)) continue;
      if (flags & OBJINVALID) {
          MK_GC_err_printf("Object with invalid pages?\n");
          continue;
      }
      MK_GC_add_roots_inner((ptr_t)O32_BASE(seg),
                         (ptr_t)(O32_BASE(seg)+O32_SIZE(seg)), FALSE);
    }
}

# else /* !OS2 */

# if defined(GWW_VDB)
#   ifndef MEM_WRITE_WATCH
#     define MEM_WRITE_WATCH 0x200000
#   endif
#   ifndef WRITE_WATCH_FLAG_RESET
#     define WRITE_WATCH_FLAG_RESET 1
#   endif

    /* Since we can't easily check whether ULONG_PTR and SIZE_T are     */
    /* defined in Win32 basetsd.h, we define own ULONG_PTR.             */
#   define MK_GC_ULONG_PTR word

    typedef UINT (WINAPI * GetWriteWatch_type)(
                                DWORD, PVOID, MK_GC_ULONG_PTR /* SIZE_T */,
                                PVOID *, MK_GC_ULONG_PTR *, PULONG);
    static GetWriteWatch_type GetWriteWatch_func;
    static DWORD GetWriteWatch_alloc_flag;

#   define MK_GC_GWW_AVAILABLE() (GetWriteWatch_func != NULL)

    static void detect_GetWriteWatch(void)
    {
      static MK_GC_bool done;
      HMODULE hK32;
      if (done)
        return;

#     if defined(MPROTECT_VDB)
        {
          char * str = GETENV("MK_GC_USE_GETWRITEWATCH");
#         if defined(MK_GC_PREFER_MPROTECT_VDB)
            if (str == NULL || (*str == '0' && *(str + 1) == '\0')) {
              /* MK_GC_USE_GETWRITEWATCH is unset or set to "0".           */
              done = TRUE; /* falling back to MPROTECT_VDB strategy.    */
              /* This should work as if GWW_VDB is undefined. */
              return;
            }
#         else
            if (str != NULL && *str == '0' && *(str + 1) == '\0') {
              /* MK_GC_USE_GETWRITEWATCH is set "0".                       */
              done = TRUE; /* falling back to MPROTECT_VDB strategy.    */
              return;
            }
#         endif
        }
#     endif

      hK32 = GetModuleHandle(TEXT("kernel32.dll"));
      if (hK32 != (HMODULE)0 &&
          (GetWriteWatch_func = (GetWriteWatch_type)(void *)GetProcAddress(hK32, "GetWriteWatch")) != NULL) {
        /* Also check whether VirtualAlloc accepts MEM_WRITE_WATCH,   */
        /* as some versions of kernel32.dll have one but not the      */
        /* other, making the feature completely broken.               */
        void * page = VirtualAlloc(NULL, MK_GC_page_size,
                                    MEM_WRITE_WATCH | MEM_RESERVE,
                                    PAGE_READWRITE);
        if (page != NULL) {
          PVOID pages[16];
          MK_GC_ULONG_PTR count = 16;
          DWORD page_size;
          /* Check that it actually works.  In spite of some            */
          /* documentation it actually seems to exist on W2K.           */
          /* This test may be unnecessary, but ...                      */
          if (GetWriteWatch_func(WRITE_WATCH_FLAG_RESET,
                                 page, MK_GC_page_size,
                                 pages,
                                 &count,
                                 &page_size) != 0) {
            /* GetWriteWatch always fails. */
            GetWriteWatch_func = NULL;
          } else {
            GetWriteWatch_alloc_flag = MEM_WRITE_WATCH;
          }
          VirtualFree(page, 0 /* dwSize */, MEM_RELEASE);
        } else {
          /* GetWriteWatch will be useless. */
          GetWriteWatch_func = NULL;
        }
      }
#     ifndef SMALL_CONFIG
        if (GetWriteWatch_func == NULL) {
          MK_GC_COND_LOG_PRINTF("Did not find a usable GetWriteWatch()\n");
        } else {
          MK_GC_COND_LOG_PRINTF("Using GetWriteWatch()\n");
        }
#     endif
      done = TRUE;
    }

# else
#   define GetWriteWatch_alloc_flag 0
# endif /* !GWW_VDB */

# if defined(MSWIN32) || defined(MSWINCE) || defined(CYGWIN32)

# ifdef MSWIN32
  /* Unfortunately, we have to handle win32s very differently from NT,  */
  /* Since VirtualQuery has very different semantics.  In particular,   */
  /* under win32s a VirtualQuery call on an unmapped page returns an    */
  /* invalid result.  Under NT, MK_GC_register_data_segments is a no-op    */
  /* and all real work is done by MK_GC_register_dynamic_libraries.  Under */
  /* win32s, we cannot find the data segments associated with dll's.    */
  /* We register the main data segment here.                            */
  MK_GC_INNER MK_GC_bool MK_GC_no_win32_dlls = FALSE;
        /* This used to be set for gcc, to avoid dealing with           */
        /* the structured exception handling issues.  But we now have   */
        /* assembly code to do that right.                              */

  MK_GC_INNER MK_GC_bool MK_GC_wnt = FALSE;
         /* This is a Windows NT derivative, i.e. NT, W2K, XP or later. */

  MK_GC_INNER void MK_GC_init_win32(void)
  {
    /* Set MK_GC_wnt.  If we're running under win32s, assume that no DLLs  */
    /* will be loaded.  I doubt anyone still runs win32s, but...        */
    DWORD v = GetVersion();
    MK_GC_wnt = !(v & 0x80000000);
    MK_GC_no_win32_dlls |= ((!MK_GC_wnt) && (v & 0xff) <= 3);
#   ifdef USE_MUNMAP
      if (MK_GC_no_win32_dlls) {
        /* Turn off unmapping for safety (since may not work well with  */
        /* GlobalAlloc).                                                */
        MK_GC_unmap_threshold = 0;
      }
#   endif
  }

  /* Return the smallest address a such that VirtualQuery               */
  /* returns correct results for all addresses between a and start.     */
  /* Assumes VirtualQuery returns correct information for start.        */
  STATIC ptr_t MK_GC_least_described_address(ptr_t start)
  {
    MEMORY_BASIC_INFORMATION buf;
    size_t result;
    LPVOID limit;
    ptr_t p;
    LPVOID q;

    limit = MK_GC_sysinfo.lpMinimumApplicationAddress;
    p = (ptr_t)((word)start & ~(MK_GC_page_size - 1));
    for (;;) {
        q = (LPVOID)(p - MK_GC_page_size);
        if ((word)q > (word)p /* underflow */ || (word)q < (word)limit) break;
        result = VirtualQuery(q, &buf, sizeof(buf));
        if (result != sizeof(buf) || buf.AllocationBase == 0) break;
        p = (ptr_t)(buf.AllocationBase);
    }
    return p;
  }
# endif /* MSWIN32 */

# ifndef REDIRECT_MALLOC
  /* We maintain a linked list of AllocationBase values that we know    */
  /* correspond to malloc heap sections.  Currently this is only called */
  /* during a GC.  But there is some hope that for long running         */
  /* programs we will eventually see most heap sections.                */

  /* In the long run, it would be more reliable to occasionally walk    */
  /* the malloc heap with HeapWalk on the default heap.  But that       */
  /* apparently works only for NT-based Windows.                        */

  STATIC size_t MK_GC_max_root_size = 100000; /* Appr. largest root size.  */

# ifdef USE_WINALLOC
  /* In the long run, a better data structure would also be nice ...    */
  STATIC struct MK_GC_malloc_heap_list {
    void * allocation_base;
    struct MK_GC_malloc_heap_list *next;
  } *MK_GC_malloc_heap_l = 0;

  /* Is p the base of one of the malloc heap sections we already know   */
  /* about?                                                             */
  STATIC MK_GC_bool MK_GC_is_malloc_heap_base(ptr_t p)
  {
    struct MK_GC_malloc_heap_list *q = MK_GC_malloc_heap_l;

    while (0 != q) {
      if (q -> allocation_base == p) return TRUE;
      q = q -> next;
    }
    return FALSE;
  }

  STATIC void *MK_GC_get_allocation_base(void *p)
  {
    MEMORY_BASIC_INFORMATION buf;
    size_t result = VirtualQuery(p, &buf, sizeof(buf));
    if (result != sizeof(buf)) {
      ABORT("Weird VirtualQuery result");
    }
    return buf.AllocationBase;
  }

  MK_GC_INNER void MK_GC_add_current_malloc_heap(void)
  {
    struct MK_GC_malloc_heap_list *new_l =
                 malloc(sizeof(struct MK_GC_malloc_heap_list));
    void * candidate = MK_GC_get_allocation_base(new_l);

    if (new_l == 0) return;
    if (MK_GC_is_malloc_heap_base(candidate)) {
      /* Try a little harder to find malloc heap.                       */
        size_t req_size = 10000;
        do {
          void *p = malloc(req_size);
          if (0 == p) {
            free(new_l);
            return;
          }
          candidate = MK_GC_get_allocation_base(p);
          free(p);
          req_size *= 2;
        } while (MK_GC_is_malloc_heap_base(candidate)
                 && req_size < MK_GC_max_root_size/10 && req_size < 500000);
        if (MK_GC_is_malloc_heap_base(candidate)) {
          free(new_l);
          return;
        }
    }
    MK_GC_COND_LOG_PRINTF("Found new system malloc AllocationBase at %p\n",
                       candidate);
    new_l -> allocation_base = candidate;
    new_l -> next = MK_GC_malloc_heap_l;
    MK_GC_malloc_heap_l = new_l;
  }
# endif /* USE_WINALLOC */

# endif /* !REDIRECT_MALLOC */

  STATIC word MK_GC_n_heap_bases = 0;      /* See MK_GC_heap_bases.   */

  /* Is p the start of either the malloc heap, or of one of our */
  /* heap sections?                                             */
  MK_GC_INNER MK_GC_bool MK_GC_is_heap_base(ptr_t p)
  {
     unsigned i;
#    ifndef REDIRECT_MALLOC
       if (MK_GC_root_size > MK_GC_max_root_size) MK_GC_max_root_size = MK_GC_root_size;
#      ifdef USE_WINALLOC
         if (MK_GC_is_malloc_heap_base(p)) return TRUE;
#      endif
#    endif
     for (i = 0; i < MK_GC_n_heap_bases; i++) {
         if (MK_GC_heap_bases[i] == p) return TRUE;
     }
     return FALSE;
  }

#ifdef MSWIN32
  STATIC void MK_GC_register_root_section(ptr_t static_root)
  {
      MEMORY_BASIC_INFORMATION buf;
      size_t result;
      DWORD protect;
      LPVOID p;
      char * base;
      char * limit, * new_limit;

      if (!MK_GC_no_win32_dlls) return;
      p = base = limit = MK_GC_least_described_address(static_root);
      while ((word)p < (word)MK_GC_sysinfo.lpMaximumApplicationAddress) {
        result = VirtualQuery(p, &buf, sizeof(buf));
        if (result != sizeof(buf) || buf.AllocationBase == 0
            || MK_GC_is_heap_base(buf.AllocationBase)) break;
        new_limit = (char *)p + buf.RegionSize;
        protect = buf.Protect;
        if (buf.State == MEM_COMMIT
            && is_writable(protect)) {
            if ((char *)p == limit) {
                limit = new_limit;
            } else {
                if (base != limit) MK_GC_add_roots_inner(base, limit, FALSE);
                base = p;
                limit = new_limit;
            }
        }
        if ((word)p > (word)new_limit /* overflow */) break;
        p = (LPVOID)new_limit;
      }
      if (base != limit) MK_GC_add_roots_inner(base, limit, FALSE);
  }
#endif /* MSWIN32 */

  void MK_GC_register_data_segments(void)
  {
#   ifdef MSWIN32
      MK_GC_register_root_section((ptr_t)&MK_GC_pages_executable);
                            /* any other GC global variable would fit too. */
#   endif
  }

# else /* !OS2 && !Windows */

# if (defined(SVR4) || defined(AUX) || defined(DGUX) \
      || (defined(LINUX) && defined(SPARC))) && !defined(PCR)
  ptr_t MK_GC_SysVGetDataStart(size_t max_page_size, ptr_t etext_addr)
  {
    word text_end = ((word)(etext_addr) + sizeof(word) - 1)
                    & ~(sizeof(word) - 1);
        /* etext rounded to word boundary       */
    word next_page = ((text_end + (word)max_page_size - 1)
                      & ~((word)max_page_size - 1));
    word page_offset = (text_end & ((word)max_page_size - 1));
    char * volatile result = (char *)(next_page + page_offset);
    /* Note that this isn't equivalent to just adding           */
    /* max_page_size to &etext if &etext is at a page boundary  */

    MK_GC_setup_temporary_fault_handler();
    if (SETJMP(MK_GC_jmp_buf) == 0) {
        /* Try writing to the address.  */
        *result = *result;
        MK_GC_reset_fault_handler();
    } else {
        MK_GC_reset_fault_handler();
        /* We got here via a longjmp.  The address is not readable.     */
        /* This is known to happen under Solaris 2.4 + gcc, which place */
        /* string constants in the text segment, but after etext.       */
        /* Use plan B.  Note that we now know there is a gap between    */
        /* text and data segments, so plan A bought us something.       */
        result = (char *)MK_GC_find_limit((ptr_t)(DATAEND), FALSE);
    }
    return((ptr_t)result);
  }
# endif

# if defined(FREEBSD) && !defined(PCR) && (defined(I386) || defined(X86_64) \
                                || defined(powerpc) || defined(__powerpc__))

/* Its unclear whether this should be identical to the above, or        */
/* whether it should apply to non-X86 architectures.                    */
/* For now we don't assume that there is always an empty page after     */
/* etext.  But in some cases there actually seems to be slightly more.  */
/* This also deals with holes between read-only data and writable data. */
ptr_t MK_GC_FreeBSDGetDataStart(size_t max_page_size, ptr_t etext_addr)
{
    word text_end = ((word)(etext_addr) + sizeof(word) - 1)
                     & ~(sizeof(word) - 1);
        /* etext rounded to word boundary       */
    volatile word next_page = (text_end + (word)max_page_size - 1)
                              & ~((word)max_page_size - 1);
    volatile ptr_t result = (ptr_t)text_end;
    MK_GC_setup_temporary_fault_handler();
    if (SETJMP(MK_GC_jmp_buf) == 0) {
        /* Try reading at the address.                          */
        /* This should happen before there is another thread.   */
        for (; next_page < (word)(DATAEND); next_page += (word)max_page_size)
            *(volatile char *)next_page;
        MK_GC_reset_fault_handler();
    } else {
        MK_GC_reset_fault_handler();
        /* As above, we go to plan B    */
        result = MK_GC_find_limit((ptr_t)(DATAEND), FALSE);
    }
    return(result);
}

# endif /* FREEBSD */


#ifdef AMIGA

#  define MK_GC_AMIGA_DS
#  include "extra/AmigaOS.c"
#  undef MK_GC_AMIGA_DS

#elif defined(OPENBSD)

/* Depending on arch alignment, there can be multiple holes     */
/* between DATASTART and DATAEND.  Scan in DATASTART .. DATAEND */
/* and register each region.                                    */
void MK_GC_register_data_segments(void)
{
  ptr_t region_start = DATASTART;
  ptr_t region_end;

  for (;;) {
    region_end = MK_GC_find_limit_openbsd(region_start, DATAEND);
    MK_GC_add_roots_inner(region_start, region_end, FALSE);
    if ((word)region_end >= (word)(DATAEND))
      break;
    region_start = MK_GC_skip_hole_openbsd(region_end, DATAEND);
  }
}

# else /* !OS2 && !Windows && !AMIGA && !OPENBSD */

void MK_GC_register_data_segments(void)
{
#   if !defined(PCR) && !defined(MACOS)
#     if defined(REDIRECT_MALLOC) && defined(MK_GC_SOLARIS_THREADS)
        /* As of Solaris 2.3, the Solaris threads implementation        */
        /* allocates the data structure for the initial thread with     */
        /* sbrk at process startup.  It needs to be scanned, so that    */
        /* we don't lose some malloc allocated data structures          */
        /* hanging from it.  We're on thin ice here ...                 */
        extern caddr_t sbrk(int);

        MK_GC_ASSERT(DATASTART);
        {
          ptr_t p = (ptr_t)sbrk(0);
          if ((word)(DATASTART) < (word)p)
            MK_GC_add_roots_inner(DATASTART, p, FALSE);
        }
#     else
        MK_GC_ASSERT(DATASTART);
        MK_GC_add_roots_inner(DATASTART, (ptr_t)(DATAEND), FALSE);
#       if defined(DATASTART2)
          MK_GC_add_roots_inner(DATASTART2, (ptr_t)(DATAEND2), FALSE);
#       endif
#     endif
#   endif
#   if defined(MACOS)
    {
#   if defined(THINK_C)
        extern void* MK_GC_MacGetDataStart(void);
        /* globals begin above stack and end at a5. */
        MK_GC_add_roots_inner((ptr_t)MK_GC_MacGetDataStart(),
                           (ptr_t)LMGetCurrentA5(), FALSE);
#   else
#     if defined(__MWERKS__)
#       if !__POWERPC__
          extern void* MK_GC_MacGetDataStart(void);
          /* MATTHEW: Function to handle Far Globals (CW Pro 3) */
#         if __option(far_data)
          extern void* MK_GC_MacGetDataEnd(void);
#         endif
          /* globals begin above stack and end at a5. */
          MK_GC_add_roots_inner((ptr_t)MK_GC_MacGetDataStart(),
                             (ptr_t)LMGetCurrentA5(), FALSE);
          /* MATTHEW: Handle Far Globals */
#         if __option(far_data)
      /* Far globals follow he QD globals: */
          MK_GC_add_roots_inner((ptr_t)LMGetCurrentA5(),
                             (ptr_t)MK_GC_MacGetDataEnd(), FALSE);
#         endif
#       else
          extern char __data_start__[], __data_end__[];
          MK_GC_add_roots_inner((ptr_t)&__data_start__,
                             (ptr_t)&__data_end__, FALSE);
#       endif /* __POWERPC__ */
#     endif /* __MWERKS__ */
#   endif /* !THINK_C */
    }
#   endif /* MACOS */

    /* Dynamic libraries are added at every collection, since they may  */
    /* change.                                                          */
}

# endif /* !AMIGA */
# endif /* !MSWIN32 && !MSWINCE */
# endif /* !OS2 */

/*
 * Auxiliary routines for obtaining memory from OS.
 */

# if !defined(OS2) && !defined(PCR) && !defined(AMIGA) \
     && !defined(USE_WINALLOC) && !defined(MACOS) && !defined(DOS4GW) \
     && !defined(NONSTOP) && !defined(SN_TARGET_PS3) && !defined(RTEMS) \
     && !defined(__CC_ARM)

# define SBRK_ARG_T ptrdiff_t

#if defined(MMAP_SUPPORTED)

#ifdef USE_MMAP_FIXED
#   define MK_GC_MMAP_FLAGS MAP_FIXED | MAP_PRIVATE
        /* Seems to yield better performance on Solaris 2, but can      */
        /* be unreliable if something is already mapped at the address. */
#else
#   define MK_GC_MMAP_FLAGS MAP_PRIVATE
#endif

#ifdef USE_MMAP_ANON
# define zero_fd -1
# if defined(MAP_ANONYMOUS)
#   define OPT_MAP_ANON MAP_ANONYMOUS
# else
#   define OPT_MAP_ANON MAP_ANON
# endif
#else
  static int zero_fd;
# define OPT_MAP_ANON 0
#endif

#ifndef HEAP_START
#   define HEAP_START ((ptr_t)0)
#endif

#ifdef SYMBIAN
  extern char* MK_GC_get_private_path_and_zero_file(void);
#endif

STATIC ptr_t MK_GC_unix_mmap_get_mem(word bytes)
{
    void *result;
    static ptr_t last_addr = HEAP_START;

#   ifndef USE_MMAP_ANON
      static MK_GC_bool initialized = FALSE;

      if (!EXPECT(initialized, TRUE)) {
#       ifdef SYMBIAN
          char* path = MK_GC_get_private_path_and_zero_file();
          zero_fd = open(path, O_RDWR | O_CREAT, 0666);
          free(path);
#       else
          zero_fd = open("/dev/zero", O_RDONLY);
#       endif
          if (zero_fd == -1)
            ABORT("Could not open /dev/zero");

          fcntl(zero_fd, F_SETFD, FD_CLOEXEC);
          initialized = TRUE;
      }
#   endif

    if (bytes & (MK_GC_page_size - 1)) ABORT("Bad GET_MEM arg");
    result = mmap(last_addr, bytes, (PROT_READ | PROT_WRITE)
                                    | (MK_GC_pages_executable ? PROT_EXEC : 0),
                  MK_GC_MMAP_FLAGS | OPT_MAP_ANON, zero_fd, 0/* offset */);
#   undef IGNORE_PAGES_EXECUTABLE

    if (result == MAP_FAILED) return(0);
    last_addr = (ptr_t)result + bytes + MK_GC_page_size - 1;
    last_addr = (ptr_t)((word)last_addr & ~(MK_GC_page_size - 1));
#   if !defined(LINUX)
      if (last_addr == 0) {
        /* Oops.  We got the end of the address space.  This isn't      */
        /* usable by arbitrary C code, since one-past-end pointers      */
        /* don't work, so we discard it and try again.                  */
        munmap(result, (size_t)(-MK_GC_page_size) - (size_t)result);
                        /* Leave last page mapped, so we can't repeat.  */
        return MK_GC_unix_mmap_get_mem(bytes);
      }
#   else
      MK_GC_ASSERT(last_addr != 0);
#   endif
    if (((word)result % HBLKSIZE) != 0)
      ABORT(
       "MK_GC_unix_get_mem: Memory returned by mmap is not aligned to HBLKSIZE.");
    return((ptr_t)result);
}

# endif  /* MMAP_SUPPORTED */

#if defined(USE_MMAP)
  ptr_t MK_GC_unix_get_mem(word bytes)
  {
    return MK_GC_unix_mmap_get_mem(bytes);
  }
#else /* !USE_MMAP */

STATIC ptr_t MK_GC_unix_sbrk_get_mem(word bytes)
{
  ptr_t result;
# ifdef IRIX5
    /* Bare sbrk isn't thread safe.  Play by malloc rules.      */
    /* The equivalent may be needed on other systems as well.   */
    __LOCK_MALLOC();
# endif
  {
    ptr_t cur_brk = (ptr_t)sbrk(0);
    SBRK_ARG_T lsbs = (word)cur_brk & (MK_GC_page_size-1);

    if ((SBRK_ARG_T)bytes < 0) {
        result = 0; /* too big */
        goto out;
    }
    if (lsbs != 0) {
        if((ptr_t)sbrk(MK_GC_page_size - lsbs) == (ptr_t)(-1)) {
            result = 0;
            goto out;
        }
    }
#   ifdef ADD_HEAP_GUARD_PAGES
      /* This is useful for catching severe memory overwrite problems that */
      /* span heap sections.  It shouldn't otherwise be turned on.         */
      {
        ptr_t guard = (ptr_t)sbrk((SBRK_ARG_T)MK_GC_page_size);
        if (mprotect(guard, MK_GC_page_size, PROT_NONE) != 0)
            ABORT("ADD_HEAP_GUARD_PAGES: mprotect failed");
      }
#   endif /* ADD_HEAP_GUARD_PAGES */
    result = (ptr_t)sbrk((SBRK_ARG_T)bytes);
    if (result == (ptr_t)(-1)) result = 0;
  }
 out:
# ifdef IRIX5
    __UNLOCK_MALLOC();
# endif
  return(result);
}

ptr_t MK_GC_unix_get_mem(word bytes)
{
#if __linux && defined(THREADS)
#error "BDWGC will use sbrk() on Linux which is almost surely wrong since it is NOT thread-safe " \
       "and most likely to clash with malloc() et al!"
#endif
# if defined(MMAP_SUPPORTED)
    /* By default, we try both sbrk and mmap, in that order.    */
    static MK_GC_bool sbrk_failed = FALSE;
    ptr_t result = 0;

    if (!sbrk_failed) result = MK_GC_unix_sbrk_get_mem(bytes);
    if (0 == result) {
        sbrk_failed = TRUE;
        result = MK_GC_unix_mmap_get_mem(bytes);
    }
    if (0 == result) {
        /* Try sbrk again, in case sbrk memory became available.        */
        result = MK_GC_unix_sbrk_get_mem(bytes);
    }
    return result;
# else /* !MMAP_SUPPORTED */
    return MK_GC_unix_sbrk_get_mem(bytes);
# endif
}

#endif /* !USE_MMAP */

# endif /* UN*X */

# ifdef OS2

void * os2_alloc(size_t bytes)
{
    void * result;

    if (DosAllocMem(&result, bytes, (PAG_READ | PAG_WRITE | PAG_COMMIT)
                                    | (MK_GC_pages_executable ? PAG_EXECUTE : 0))
                    != NO_ERROR) {
        return(0);
    }
    /* FIXME: What's the purpose of this recursion?  (Probably, if      */
    /* DosAllocMem returns memory at 0 address then just retry once.)   */
    if (result == 0) return(os2_alloc(bytes));
    return(result);
}

# endif /* OS2 */

#ifdef MSWINCE
  ptr_t MK_GC_wince_get_mem(word bytes)
  {
    ptr_t result = 0; /* initialized to prevent warning. */
    word i;

    /* Round up allocation size to multiple of page size */
    bytes = (bytes + MK_GC_page_size-1) & ~(MK_GC_page_size-1);

    /* Try to find reserved, uncommitted pages */
    for (i = 0; i < MK_GC_n_heap_bases; i++) {
        if (((word)(-(signed_word)MK_GC_heap_lengths[i])
             & (MK_GC_sysinfo.dwAllocationGranularity-1))
            >= bytes) {
            result = MK_GC_heap_bases[i] + MK_GC_heap_lengths[i];
            break;
        }
    }

    if (i == MK_GC_n_heap_bases) {
        /* Reserve more pages */
        word res_bytes = (bytes + MK_GC_sysinfo.dwAllocationGranularity-1)
                         & ~(MK_GC_sysinfo.dwAllocationGranularity-1);
        /* If we ever support MPROTECT_VDB here, we will probably need to    */
        /* ensure that res_bytes is strictly > bytes, so that VirtualProtect */
        /* never spans regions.  It seems to be OK for a VirtualFree         */
        /* argument to span regions, so we should be OK for now.             */
        result = (ptr_t) VirtualAlloc(NULL, res_bytes,
                                MEM_RESERVE | MEM_TOP_DOWN,
                                MK_GC_pages_executable ? PAGE_EXECUTE_READWRITE :
                                                      PAGE_READWRITE);
        if (HBLKDISPL(result) != 0) ABORT("Bad VirtualAlloc result");
            /* If I read the documentation correctly, this can  */
            /* only happen if HBLKSIZE > 64k or not a power of 2.       */
        if (MK_GC_n_heap_bases >= MAX_HEAP_SECTS) ABORT("Too many heap sections");
        if (result == NULL) return NULL;
        MK_GC_heap_bases[MK_GC_n_heap_bases] = result;
        MK_GC_heap_lengths[MK_GC_n_heap_bases] = 0;
        MK_GC_n_heap_bases++;
    }

    /* Commit pages */
    result = (ptr_t) VirtualAlloc(result, bytes, MEM_COMMIT,
                              MK_GC_pages_executable ? PAGE_EXECUTE_READWRITE :
                                                    PAGE_READWRITE);
#   undef IGNORE_PAGES_EXECUTABLE

    if (result != NULL) {
        if (HBLKDISPL(result) != 0) ABORT("Bad VirtualAlloc result");
        MK_GC_heap_lengths[i] += bytes;
    }

    return(result);
  }

#elif defined(USE_WINALLOC) || defined(CYGWIN32)

# ifdef USE_GLOBAL_ALLOC
#   define GLOBAL_ALLOC_TEST 1
# else
#   define GLOBAL_ALLOC_TEST MK_GC_no_win32_dlls
# endif

# if defined(MK_GC_USE_MEM_TOP_DOWN) && defined(USE_WINALLOC)
    DWORD MK_GC_mem_top_down = MEM_TOP_DOWN;
                           /* Use MK_GC_USE_MEM_TOP_DOWN for better 64-bit */
                           /* testing.  Otherwise all addresses tend to */
                           /* end up in first 4GB, hiding bugs.         */
# else
#   define MK_GC_mem_top_down 0
# endif /* !MK_GC_USE_MEM_TOP_DOWN */

  ptr_t MK_GC_win32_get_mem(word bytes)
  {
    ptr_t result;

# ifndef USE_WINALLOC
    result = MK_GC_unix_get_mem(bytes);
# else
#   ifdef MSWIN32
      if (GLOBAL_ALLOC_TEST) {
        /* VirtualAlloc doesn't like PAGE_EXECUTE_READWRITE.    */
        /* There are also unconfirmed rumors of other           */
        /* problems, so we dodge the issue.                     */
        result = (ptr_t) GlobalAlloc(0, bytes + HBLKSIZE);
        result = (ptr_t)(((word)result + HBLKSIZE - 1) & ~(HBLKSIZE-1));
      } else
#   endif
    /* else */ {
        /* VirtualProtect only works on regions returned by a   */
        /* single VirtualAlloc call.  Thus we allocate one      */
        /* extra page, which will prevent merging of blocks     */
        /* in separate regions, and eliminate any temptation    */
        /* to call VirtualProtect on a range spanning regions.  */
        /* This wastes a small amount of memory, and risks      */
        /* increased fragmentation.  But better alternatives    */
        /* would require effort.                                */
#       ifdef MPROTECT_VDB
          /* We can't check for MK_GC_incremental here (because    */
          /* MK_GC_enable_incremental() might be called some time  */
          /* later after the GC initialization).                */
#         ifdef GWW_VDB
#           define VIRTUAL_ALLOC_PAD (MK_GC_GWW_AVAILABLE() ? 0 : 1)
#         else
#           define VIRTUAL_ALLOC_PAD 1
#         endif
#       else
#         define VIRTUAL_ALLOC_PAD 0
#       endif
        /* Pass the MEM_WRITE_WATCH only if GetWriteWatch-based */
        /* VDBs are enabled and the GetWriteWatch function is   */
        /* available.  Otherwise we waste resources or possibly */
        /* cause VirtualAlloc to fail (observed in Windows 2000 */
        /* SP2).                                                */
        result = (ptr_t) VirtualAlloc(NULL, bytes + VIRTUAL_ALLOC_PAD,
                                GetWriteWatch_alloc_flag
                                | (MEM_COMMIT | MEM_RESERVE)
                                | MK_GC_mem_top_down,
                                MK_GC_pages_executable ? PAGE_EXECUTE_READWRITE :
                                                      PAGE_READWRITE);
#       undef IGNORE_PAGES_EXECUTABLE
    }
# endif /* USE_WINALLOC */
    if (HBLKDISPL(result) != 0) ABORT("Bad VirtualAlloc result");
        /* If I read the documentation correctly, this can      */
        /* only happen if HBLKSIZE > 64k or not a power of 2.   */
    if (MK_GC_n_heap_bases >= MAX_HEAP_SECTS) ABORT("Too many heap sections");
    if (0 != result) MK_GC_heap_bases[MK_GC_n_heap_bases++] = result;
    return(result);
  }

  MK_GC_API void MK_GC_CALL MK_GC_win32_free_heap(void)
  {
#   ifndef CYGWIN32
      if (GLOBAL_ALLOC_TEST)
#   endif
    {
      while (MK_GC_n_heap_bases-- > 0) {
#       ifdef CYGWIN32
          /* FIXME: Is it OK to use non-GC free() here? */
#       else
          GlobalFree(MK_GC_heap_bases[MK_GC_n_heap_bases]);
#       endif
        MK_GC_heap_bases[MK_GC_n_heap_bases] = 0;
      }
    } /* else */
#   ifndef CYGWIN32
      else {
        /* Avoiding VirtualAlloc leak. */
        while (MK_GC_n_heap_bases > 0) {
          VirtualFree(MK_GC_heap_bases[--MK_GC_n_heap_bases], 0, MEM_RELEASE);
          MK_GC_heap_bases[MK_GC_n_heap_bases] = 0;
        }
      }
#   endif
  }
#endif /* USE_WINALLOC || CYGWIN32 */

#ifdef AMIGA
# define MK_GC_AMIGA_AM
# include "extra/AmigaOS.c"
# undef MK_GC_AMIGA_AM
#endif

#ifdef USE_MUNMAP

/* For now, this only works on Win32/WinCE and some Unix-like   */
/* systems.  If you have something else, don't define           */
/* USE_MUNMAP.                                                  */

#if !defined(MSWIN32) && !defined(MSWINCE)
# include <unistd.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <sys/types.h>
#endif

/* Compute a page aligned starting address for the unmap        */
/* operation on a block of size bytes starting at start.        */
/* Return 0 if the block is too small to make this feasible.    */
STATIC ptr_t MK_GC_unmap_start(ptr_t start, size_t bytes)
{
    ptr_t result;
    /* Round start to next page boundary.       */
    result = (ptr_t)((word)(start + MK_GC_page_size - 1) & ~(MK_GC_page_size - 1));
    if ((word)(result + MK_GC_page_size) > (word)(start + bytes)) return 0;
    return result;
}

/* Compute end address for an unmap operation on the indicated  */
/* block.                                                       */
STATIC ptr_t MK_GC_unmap_end(ptr_t start, size_t bytes)
{
    return (ptr_t)((word)(start + bytes) & ~(MK_GC_page_size - 1));
}

/* Under Win32/WinCE we commit (map) and decommit (unmap)       */
/* memory using VirtualAlloc and VirtualFree.  These functions  */
/* work on individual allocations of virtual memory, made       */
/* previously using VirtualAlloc with the MEM_RESERVE flag.     */
/* The ranges we need to (de)commit may span several of these   */
/* allocations; therefore we use VirtualQuery to check          */
/* allocation lengths, and split up the range as necessary.     */

/* We assume that MK_GC_remap is called on exactly the same range  */
/* as a previous call to MK_GC_unmap.  It is safe to consistently  */
/* round the endpoints in both places.                          */
MK_GC_INNER void MK_GC_unmap(ptr_t start, size_t bytes)
{
    ptr_t start_addr = MK_GC_unmap_start(start, bytes);
    ptr_t end_addr = MK_GC_unmap_end(start, bytes);
    word len = end_addr - start_addr;

    if (0 == start_addr) return;
#   ifdef USE_WINALLOC
      while (len != 0) {
          MEMORY_BASIC_INFORMATION mem_info;
          MK_GC_word free_len;

          if (VirtualQuery(start_addr, &mem_info, sizeof(mem_info))
              != sizeof(mem_info))
              ABORT("Weird VirtualQuery result");
          free_len = (len < mem_info.RegionSize) ? len : mem_info.RegionSize;
          if (!VirtualFree(start_addr, free_len, MEM_DECOMMIT))
              ABORT("VirtualFree failed");
          MK_GC_unmapped_bytes += free_len;
          start_addr += free_len;
          len -= free_len;
      }
#   else
      /* We immediately remap it to prevent an intervening mmap from    */
      /* accidentally grabbing the same address space.                  */
      {
        void * result;

        result = mmap(start_addr, len, PROT_NONE,
                      MAP_PRIVATE | MAP_FIXED | OPT_MAP_ANON,
                      zero_fd, 0/* offset */);
        if (result != (void *)start_addr)
          ABORT("mmap(PROT_NONE) failed");
      }
      MK_GC_unmapped_bytes += len;
#   endif
}

MK_GC_INNER void MK_GC_remap(ptr_t start, size_t bytes)
{
    ptr_t start_addr = MK_GC_unmap_start(start, bytes);
    ptr_t end_addr = MK_GC_unmap_end(start, bytes);
    word len = end_addr - start_addr;
    if (0 == start_addr) return;

    /* FIXME: Handle out-of-memory correctly (at least for Win32)       */
#   ifdef USE_WINALLOC
      while (len != 0) {
          MEMORY_BASIC_INFORMATION mem_info;
          MK_GC_word alloc_len;
          ptr_t result;

          if (VirtualQuery(start_addr, &mem_info, sizeof(mem_info))
              != sizeof(mem_info))
              ABORT("Weird VirtualQuery result");
          alloc_len = (len < mem_info.RegionSize) ? len : mem_info.RegionSize;
          result = VirtualAlloc(start_addr, alloc_len, MEM_COMMIT,
                                MK_GC_pages_executable ? PAGE_EXECUTE_READWRITE :
                                                      PAGE_READWRITE);
          if (result != start_addr) {
              if (GetLastError() == ERROR_NOT_ENOUGH_MEMORY ||
                  GetLastError() == ERROR_OUTOFMEMORY) {
                  ABORT("Not enough memory to process remapping");
              } else {
                  ABORT("VirtualAlloc remapping failed");
              }
          }
          MK_GC_unmapped_bytes -= alloc_len;
          start_addr += alloc_len;
          len -= alloc_len;
      }
#   else
      /* It was already remapped with PROT_NONE. */
      {
#       ifdef NACL
          /* NaCl does not expose mprotect, but mmap should work fine.  */
          void *mmap_result = mmap(start_addr, len, (PROT_READ | PROT_WRITE)
                                    | (MK_GC_pages_executable ? PROT_EXEC : 0),
                                   MAP_PRIVATE | MAP_FIXED | OPT_MAP_ANON,
                                   zero_fd, 0 /* offset */);
          if (mmap_result != (void *)start_addr)
            ABORT("mmap as mprotect failed");
#       else
          if (mprotect(start_addr, len, (PROT_READ | PROT_WRITE)
                            | (MK_GC_pages_executable ? PROT_EXEC : 0)) != 0) {
            ABORT_ARG3("mprotect remapping failed",
                       " at %p (length %lu), errcode= %d",
                       start_addr, (unsigned long)len, errno);
          }
#       endif /* !NACL */
      }
#     undef IGNORE_PAGES_EXECUTABLE
      MK_GC_unmapped_bytes -= len;
#   endif
}

/* Two adjacent blocks have already been unmapped and are about to      */
/* be merged.  Unmap the whole block.  This typically requires          */
/* that we unmap a small section in the middle that was not previously  */
/* unmapped due to alignment constraints.                               */
MK_GC_INNER void MK_GC_unmap_gap(ptr_t start1, size_t bytes1, ptr_t start2,
                           size_t bytes2)
{
    ptr_t start1_addr = MK_GC_unmap_start(start1, bytes1);
    ptr_t end1_addr = MK_GC_unmap_end(start1, bytes1);
    ptr_t start2_addr = MK_GC_unmap_start(start2, bytes2);
    ptr_t start_addr = end1_addr;
    ptr_t end_addr = start2_addr;
    size_t len;

    MK_GC_ASSERT(start1 + bytes1 == start2);
    if (0 == start1_addr) start_addr = MK_GC_unmap_start(start1, bytes1 + bytes2);
    if (0 == start2_addr) end_addr = MK_GC_unmap_end(start1, bytes1 + bytes2);
    if (0 == start_addr) return;
    len = end_addr - start_addr;
#   ifdef USE_WINALLOC
      while (len != 0) {
          MEMORY_BASIC_INFORMATION mem_info;
          MK_GC_word free_len;

          if (VirtualQuery(start_addr, &mem_info, sizeof(mem_info))
              != sizeof(mem_info))
              ABORT("Weird VirtualQuery result");
          free_len = (len < mem_info.RegionSize) ? len : mem_info.RegionSize;
          if (!VirtualFree(start_addr, free_len, MEM_DECOMMIT))
              ABORT("VirtualFree failed");
          MK_GC_unmapped_bytes += free_len;
          start_addr += free_len;
          len -= free_len;
      }
#   else
      if (len != 0) {
        /* Immediately remap as above. */
        void * result;
        result = mmap(start_addr, len, PROT_NONE,
                      MAP_PRIVATE | MAP_FIXED | OPT_MAP_ANON,
                      zero_fd, 0/* offset */);
        if (result != (void *)start_addr)
          ABORT("mmap(PROT_NONE) failed");
      }
      MK_GC_unmapped_bytes += len;
#   endif
}

#endif /* USE_MUNMAP */

/* Routine for pushing any additional roots.  In THREADS        */
/* environment, this is also responsible for marking from       */
/* thread stacks.                                               */
#ifndef THREADS
  MK_GC_push_other_roots_proc MK_GC_push_other_roots = 0;
#else /* THREADS */

# ifdef PCR
PCR_ERes MK_GC_push_thread_stack(PCR_Th_T *t, PCR_Any dummy)
{
    struct PCR_ThCtl_TInfoRep info;
    PCR_ERes result;

    info.ti_stkLow = info.ti_stkHi = 0;
    result = PCR_ThCtl_GetInfo(t, &info);
    MK_GC_push_all_stack((ptr_t)(info.ti_stkLow), (ptr_t)(info.ti_stkHi));
    return(result);
}

/* Push the contents of an old object. We treat this as stack   */
/* data only because that makes it robust against mark stack    */
/* overflow.                                                    */
PCR_ERes MK_GC_push_old_obj(void *p, size_t size, PCR_Any data)
{
    MK_GC_push_all_stack((ptr_t)p, (ptr_t)p + size);
    return(PCR_ERes_okay);
}

extern struct PCR_MM_ProcsRep * MK_GC_old_allocator;
                                        /* defined in pcr_interface.c.  */

STATIC void MK_GC_CALLBACK MK_GC_default_push_other_roots(void)
{
    /* Traverse data allocated by previous memory managers.             */
          if ((*(MK_GC_old_allocator->mmp_enumerate))(PCR_Bool_false,
                                                   MK_GC_push_old_obj, 0)
              != PCR_ERes_okay) {
              ABORT("Old object enumeration failed");
          }
    /* Traverse all thread stacks. */
        if (PCR_ERes_IsErr(
                PCR_ThCtl_ApplyToAllOtherThreads(MK_GC_push_thread_stack,0))
            || PCR_ERes_IsErr(MK_GC_push_thread_stack(PCR_Th_CurrThread(), 0))) {
          ABORT("Thread stack marking failed");
        }
}

# endif /* PCR */

# if defined(MK_GC_PTHREADS) || defined(MK_GC_WIN32_THREADS)
    STATIC void MK_GC_CALLBACK MK_GC_default_push_other_roots(void)
    {
      MK_GC_push_all_stacks();
    }
# endif /* MK_GC_WIN32_THREADS || MK_GC_PTHREADS */

# ifdef SN_TARGET_PS3
    STATIC void MK_GC_CALLBACK MK_GC_default_push_other_roots(void)
    {
      ABORT("MK_GC_default_push_other_roots is not implemented");
    }

    void MK_GC_push_thread_structures(void)
    {
      ABORT("MK_GC_push_thread_structures is not implemented");
    }
# endif /* SN_TARGET_PS3 */

  MK_GC_push_other_roots_proc MK_GC_push_other_roots = MK_GC_default_push_other_roots;
#endif /* THREADS */

MK_GC_API void MK_GC_CALL MK_GC_set_push_other_roots(MK_GC_push_other_roots_proc fn)
{
    MK_GC_push_other_roots = fn;
}

MK_GC_API MK_GC_push_other_roots_proc MK_GC_CALL MK_GC_get_push_other_roots(void)
{
    return MK_GC_push_other_roots;
}

/*
 * Routines for accessing dirty bits on virtual pages.
 * There are six ways to maintain this information:
 * DEFAULT_VDB: A simple dummy implementation that treats every page
 *              as possibly dirty.  This makes incremental collection
 *              useless, but the implementation is still correct.
 * MANUAL_VDB:  Stacks and static data are always considered dirty.
 *              Heap pages are considered dirty if MK_GC_dirty(p) has been
 *              called on some pointer p pointing to somewhere inside
 *              an object on that page.  A MK_GC_dirty() call on a large
 *              object directly dirties only a single page, but for
 *              MANUAL_VDB we are careful to treat an object with a dirty
 *              page as completely dirty.
 *              In order to avoid races, an object must be marked dirty
 *              after it is written, and a reference to the object
 *              must be kept on a stack or in a register in the interim.
 *              With threads enabled, an object directly reachable from the
 *              stack at the time of a collection is treated as dirty.
 *              In single-threaded mode, it suffices to ensure that no
 *              collection can take place between the pointer assignment
 *              and the MK_GC_dirty() call.
 * PCR_VDB:     Use PPCRs virtual dirty bit facility.
 * PROC_VDB:    Use the /proc facility for reading dirty bits.  Only
 *              works under some SVR4 variants.  Even then, it may be
 *              too slow to be entirely satisfactory.  Requires reading
 *              dirty bits for entire address space.  Implementations tend
 *              to assume that the client is a (slow) debugger.
 * MPROTECT_VDB:Protect pages and then catch the faults to keep track of
 *              dirtied pages.  The implementation (and implementability)
 *              is highly system dependent.  This usually fails when system
 *              calls write to a protected page.  We prevent the read system
 *              call from doing so.  It is the clients responsibility to
 *              make sure that other system calls are similarly protected
 *              or write only to the stack.
 * GWW_VDB:     Use the Win32 GetWriteWatch functions, if available, to
 *              read dirty bits.  In case it is not available (because we
 *              are running on Windows 95, Windows 2000 or earlier),
 *              MPROTECT_VDB may be defined as a fallback strategy.
 */
#ifndef MK_GC_DISABLE_INCREMENTAL
  MK_GC_INNER MK_GC_bool MK_GC_dirty_maintained = FALSE;
#endif

#if defined(PROC_VDB) || defined(GWW_VDB)
  /* Add all pages in pht2 to pht1 */
  STATIC void MK_GC_or_pages(page_hash_table pht1, page_hash_table pht2)
  {
    register unsigned i;
    for (i = 0; i < PHT_SIZE; i++) pht1[i] |= pht2[i];
  }

# ifdef MPROTECT_VDB
    STATIC MK_GC_bool MK_GC_gww_page_was_dirty(struct hblk * h)
# else
    MK_GC_INNER MK_GC_bool MK_GC_page_was_dirty(struct hblk * h)
# endif
  {
    register word index;
    if (HDR(h) == 0)
      return TRUE;
    index = PHT_HASH(h);
    return get_pht_entry_from_index(MK_GC_grungy_pages, index);
  }

# if defined(CHECKSUMS) || defined(PROC_VDB)
    /* Used only if GWW_VDB. */
#   ifdef MPROTECT_VDB
      STATIC MK_GC_bool MK_GC_gww_page_was_ever_dirty(struct hblk * h)
#   else
      MK_GC_INNER MK_GC_bool MK_GC_page_was_ever_dirty(struct hblk * h)
#   endif
    {
      register word index;
      if (HDR(h) == 0)
        return TRUE;
      index = PHT_HASH(h);
      return get_pht_entry_from_index(MK_GC_written_pages, index);
    }
# endif /* CHECKSUMS || PROC_VDB */

# ifndef MPROTECT_VDB
    /* Ignore write hints.  They don't help us here.    */
    MK_GC_INNER void MK_GC_remove_protection(struct hblk * h MK_GC_ATTR_UNUSED,
                                       word nblocks MK_GC_ATTR_UNUSED,
                                       MK_GC_bool is_ptrfree MK_GC_ATTR_UNUSED) {}
# endif

#endif /* PROC_VDB || GWW_VDB */

#ifdef GWW_VDB

# define MK_GC_GWW_BUF_LEN (MAXHINCR * HBLKSIZE / 4096 /* X86 page size */)
  /* Still susceptible to overflow, if there are very large allocations, */
  /* and everything is dirty.                                            */
  static PVOID gww_buf[MK_GC_GWW_BUF_LEN];

# ifdef MPROTECT_VDB
    MK_GC_INNER MK_GC_bool MK_GC_gww_dirty_init(void)
    {
      detect_GetWriteWatch();
      return MK_GC_GWW_AVAILABLE();
    }
# else
    MK_GC_INNER void MK_GC_dirty_init(void)
    {
      detect_GetWriteWatch();
      MK_GC_dirty_maintained = MK_GC_GWW_AVAILABLE();
    }
# endif /* !MPROTECT_VDB */

# ifdef MPROTECT_VDB
    STATIC void MK_GC_gww_read_dirty(void)
# else
    MK_GC_INNER void MK_GC_read_dirty(void)
# endif
  {
    word i;

    BZERO(MK_GC_grungy_pages, sizeof(MK_GC_grungy_pages));

    for (i = 0; i != MK_GC_n_heap_sects; ++i) {
      MK_GC_ULONG_PTR count;

      do {
        PVOID * pages, * pages_end;
        DWORD page_size;

        pages = gww_buf;
        count = MK_GC_GWW_BUF_LEN;
        /* GetWriteWatch is documented as returning non-zero when it    */
        /* fails, but the documentation doesn't explicitly say why it   */
        /* would fail or what its behaviour will be if it fails.        */
        /* It does appear to fail, at least on recent W2K instances, if */
        /* the underlying memory was not allocated with the appropriate */
        /* flag.  This is common if MK_GC_enable_incremental is called     */
        /* shortly after GC initialization.  To avoid modifying the     */
        /* interface, we silently work around such a failure, it only   */
        /* affects the initial (small) heap allocation. If there are    */
        /* more dirty pages than will fit in the buffer, this is not    */
        /* treated as a failure; we must check the page count in the    */
        /* loop condition. Since each partial call will reset the       */
        /* status of some pages, this should eventually terminate even  */
        /* in the overflow case.                                        */
        if (GetWriteWatch_func(WRITE_WATCH_FLAG_RESET,
                               MK_GC_heap_sects[i].hs_start,
                               MK_GC_heap_sects[i].hs_bytes,
                               pages,
                               &count,
                               &page_size) != 0) {
          static int warn_count = 0;
          unsigned j;
          struct hblk * start = (struct hblk *)MK_GC_heap_sects[i].hs_start;
          static struct hblk *last_warned = 0;
          size_t nblocks = divHBLKSZ(MK_GC_heap_sects[i].hs_bytes);

          if ( i != 0 && last_warned != start && warn_count++ < 5) {
            last_warned = start;
            WARN(
              "MK_GC_gww_read_dirty unexpectedly failed at %p: "
              "Falling back to marking all pages dirty\n", start);
          }
          for (j = 0; j < nblocks; ++j) {
              word hash = PHT_HASH(start + j);
              set_pht_entry_from_index(MK_GC_grungy_pages, hash);
          }
          count = 1;  /* Done with this section. */
        } else /* succeeded */ {
          pages_end = pages + count;
          while (pages != pages_end) {
            struct hblk * h = (struct hblk *) *pages++;
            struct hblk * h_end = (struct hblk *) ((char *) h + page_size);
            do {
              set_pht_entry_from_index(MK_GC_grungy_pages, PHT_HASH(h));
            } while ((word)(++h) < (word)h_end);
          }
        }
      } while (count == MK_GC_GWW_BUF_LEN);
      /* FIXME: It's unclear from Microsoft's documentation if this loop */
      /* is useful.  We suspect the call just fails if the buffer fills  */
      /* up.  But that should still be handled correctly.                */
    }

    MK_GC_or_pages(MK_GC_written_pages, MK_GC_grungy_pages);
  }
#endif /* GWW_VDB */

#ifdef DEFAULT_VDB
  /* All of the following assume the allocation lock is held.   */

  /* The client asserts that unallocated pages in the heap are never    */
  /* written.                                                           */

  /* Initialize virtual dirty bit implementation.       */
  MK_GC_INNER void MK_GC_dirty_init(void)
  {
    MK_GC_VERBOSE_LOG_PRINTF("Initializing DEFAULT_VDB...\n");
    MK_GC_dirty_maintained = TRUE;
  }

  /* Retrieve system dirty bits for heap to a local buffer.     */
  /* Restore the systems notion of which pages are dirty.       */
  MK_GC_INNER void MK_GC_read_dirty(void) {}

  /* Is the HBLKSIZE sized page at h marked dirty in the local buffer?  */
  /* If the actual page size is different, this returns TRUE if any     */
  /* of the pages overlapping h are dirty.  This routine may err on the */
  /* side of labeling pages as dirty (and this implementation does).    */
  MK_GC_INNER MK_GC_bool MK_GC_page_was_dirty(struct hblk * h MK_GC_ATTR_UNUSED)
  {
    return(TRUE);
  }

  /* The following two routines are typically less crucial.             */
  /* They matter most with large dynamic libraries, or if we can't      */
  /* accurately identify stacks, e.g. under Solaris 2.X.  Otherwise the */
  /* following default versions are adequate.                           */
# ifdef CHECKSUMS
    /* Could any valid GC heap pointer ever have been written to this page? */
    MK_GC_INNER MK_GC_bool MK_GC_page_was_ever_dirty(struct hblk * h MK_GC_ATTR_UNUSED)
    {
      return(TRUE);
    }
# endif /* CHECKSUMS */

  /* A call that:                                         */
  /* I) hints that [h, h+nblocks) is about to be written. */
  /* II) guarantees that protection is removed.           */
  /* (I) may speed up some dirty bit implementations.     */
  /* (II) may be essential if we need to ensure that      */
  /* pointer-free system call buffers in the heap are     */
  /* not protected.                                       */
  MK_GC_INNER void MK_GC_remove_protection(struct hblk * h MK_GC_ATTR_UNUSED,
                                     word nblocks MK_GC_ATTR_UNUSED,
                                     MK_GC_bool is_ptrfree MK_GC_ATTR_UNUSED) {}
#endif /* DEFAULT_VDB */

#ifdef MANUAL_VDB
  /* Initialize virtual dirty bit implementation.       */
  MK_GC_INNER void MK_GC_dirty_init(void)
  {
    MK_GC_VERBOSE_LOG_PRINTF("Initializing MANUAL_VDB...\n");
    /* MK_GC_dirty_pages and MK_GC_grungy_pages are already cleared.  */
    MK_GC_dirty_maintained = TRUE;
  }

  /* Retrieve system dirty bits for heap to a local buffer.     */
  /* Restore the systems notion of which pages are dirty.       */
  MK_GC_INNER void MK_GC_read_dirty(void)
  {
    BCOPY((word *)MK_GC_dirty_pages, MK_GC_grungy_pages,
          (sizeof MK_GC_dirty_pages));
    BZERO((word *)MK_GC_dirty_pages, (sizeof MK_GC_dirty_pages));
  }

  /* Is the HBLKSIZE sized page at h marked dirty in the local buffer?  */
  /* If the actual page size is different, this returns TRUE if any     */
  /* of the pages overlapping h are dirty.  This routine may err on the */
  /* side of labeling pages as dirty (and this implementation does).    */
  MK_GC_INNER MK_GC_bool MK_GC_page_was_dirty(struct hblk *h)
  {
    register word index = PHT_HASH(h);
    return(HDR(h) == 0 || get_pht_entry_from_index(MK_GC_grungy_pages, index));
  }

# define async_set_pht_entry_from_index(db, index) \
                        set_pht_entry_from_index(db, index) /* for now */

  /* Mark the page containing p as dirty.  Logically, this dirties the  */
  /* entire object.                                                     */
  void MK_GC_dirty(ptr_t p)
  {
    word index = PHT_HASH(p);
    async_set_pht_entry_from_index(MK_GC_dirty_pages, index);
  }

  MK_GC_INNER void MK_GC_remove_protection(struct hblk * h MK_GC_ATTR_UNUSED,
                                     word nblocks MK_GC_ATTR_UNUSED,
                                     MK_GC_bool is_ptrfree MK_GC_ATTR_UNUSED) {}

# ifdef CHECKSUMS
    /* Could any valid GC heap pointer ever have been written to this page? */
    MK_GC_INNER MK_GC_bool MK_GC_page_was_ever_dirty(struct hblk * h MK_GC_ATTR_UNUSED)
    {
      /* FIXME - implement me.  */
      return(TRUE);
    }
# endif /* CHECKSUMS */

#endif /* MANUAL_VDB */

#ifdef MPROTECT_VDB
  /* See DEFAULT_VDB for interface descriptions.        */

  /*
   * This implementation maintains dirty bits itself by catching write
   * faults and keeping track of them.  We assume nobody else catches
   * SIGBUS or SIGSEGV.  We assume no write faults occur in system calls.
   * This means that clients must ensure that system calls don't write
   * to the write-protected heap.  Probably the best way to do this is to
   * ensure that system calls write at most to pointer-free objects in the
   * heap, and do even that only if we are on a platform on which those
   * are not protected.  Another alternative is to wrap system calls
   * (see example for read below), but the current implementation holds
   * applications.
   * We assume the page size is a multiple of HBLKSIZE.
   * We prefer them to be the same.  We avoid protecting pointer-free
   * objects only if they are the same.
   */
# ifdef DARWIN
    /* Using vm_protect (mach syscall) over mprotect (BSD syscall) seems to
       decrease the likelihood of some of the problems described below. */
#   include <mach/vm_map.h>
    STATIC mach_port_t MK_GC_task_self = 0;
#   define PROTECT(addr,len) \
        if (vm_protect(MK_GC_task_self, (vm_address_t)(addr), (vm_size_t)(len), \
                       FALSE, VM_PROT_READ \
                              | (MK_GC_pages_executable ? VM_PROT_EXECUTE : 0)) \
                == KERN_SUCCESS) {} else ABORT("vm_protect(PROTECT) failed")
#   define UNPROTECT(addr,len) \
        if (vm_protect(MK_GC_task_self, (vm_address_t)(addr), (vm_size_t)(len), \
                       FALSE, (VM_PROT_READ | VM_PROT_WRITE) \
                              | (MK_GC_pages_executable ? VM_PROT_EXECUTE : 0)) \
                == KERN_SUCCESS) {} else ABORT("vm_protect(UNPROTECT) failed")

# elif !defined(USE_WINALLOC)
#   include <sys/mman.h>
#   include <signal.h>
#   include <sys/syscall.h>

#   define PROTECT(addr, len) \
        if (mprotect((caddr_t)(addr), (size_t)(len), \
                     PROT_READ \
                     | (MK_GC_pages_executable ? PROT_EXEC : 0)) >= 0) { \
        } else ABORT("mprotect failed")
#   define UNPROTECT(addr, len) \
        if (mprotect((caddr_t)(addr), (size_t)(len), \
                     (PROT_READ | PROT_WRITE) \
                     | (MK_GC_pages_executable ? PROT_EXEC : 0)) >= 0) { \
        } else ABORT(MK_GC_pages_executable ? \
                                "un-mprotect executable page failed" \
                                    " (probably disabled by OS)" : \
                                "un-mprotect failed")
#   undef IGNORE_PAGES_EXECUTABLE

# else /* USE_WINALLOC */
#   ifndef MSWINCE
#     include <signal.h>
#   endif

    static DWORD protect_junk;
#   define PROTECT(addr, len) \
        if (VirtualProtect((addr), (len), \
                           MK_GC_pages_executable ? PAGE_EXECUTE_READ : \
                                                 PAGE_READONLY, \
                           &protect_junk)) { \
        } else ABORT_ARG1("VirtualProtect failed", \
                          ": errcode= 0x%X", (unsigned)GetLastError())
#   define UNPROTECT(addr, len) \
        if (VirtualProtect((addr), (len), \
                           MK_GC_pages_executable ? PAGE_EXECUTE_READWRITE : \
                                                 PAGE_READWRITE, \
                           &protect_junk)) { \
        } else ABORT("un-VirtualProtect failed")
# endif /* USE_WINALLOC */

# if defined(MSWIN32)
#if 0
    typedef LPTOP_LEVEL_EXCEPTION_FILTER SIG_HNDLR_PTR;
#   undef SIG_DFL
#   define SIG_DFL (LPTOP_LEVEL_EXCEPTION_FILTER)((signed_word)-1)
#else
    typedef LPTOP_LEVEL_EXCEPTION_FILTER EXCEPTION_HANDLER_PTR;
    EXCEPTION_HANDLER_PTR MK_GC_old_exception_handler = NULL;
#endif
# elif defined(MSWINCE)
#if 0
    typedef LONG (WINAPI *SIG_HNDLR_PTR)(struct _EXCEPTION_POINTERS *);
#   undef SIG_DFL
#   define SIG_DFL (SIG_HNDLR_PTR) (-1)
#else
    typedef LONG (WINAPI *EXCEPTION_HANDLER_PTR)(struct _EXCEPTION_POINTERS *);
    EXCEPTION_HANDLER_PTR MK_GC_old_exception_handler = NULL;
#endif
# elif defined(DARWIN)
    typedef void (* SIG_HNDLR_PTR)();
# else
    typedef void (* SIG_HNDLR_PTR)(int, siginfo_t *, void *);
    typedef void (* PLAIN_HNDLR_PTR)(int);
struct old_sig_handler {
  /* PLAIN_HNDLR_PTR sa_handler; */
  void (* _sa_handler)(int);
  /* SIG_HNDLR_PTR sa_sigaction; */
  void (* _sa_sigaction)(int, siginfo_t *, void *);
  MK_GC_bool used_SIGINFO;
};
# endif

# if defined(__GLIBC__)
#   if __GLIBC__ < 2 || __GLIBC__ == 2 && __GLIBC_MINOR__ < 2
#       error glibc too old?
#   endif
# endif

#ifndef DARWIN
# if !defined(MSWIN32) && !defined(MSWINCE)
STATIC struct old_sig_handler MK_GC_old_segv_handler = { 0, 0, FALSE };
STATIC struct old_sig_handler MK_GC_old_bus_handler = { 0, 0, FALSE };
# endif /* !MSWIN32 */
#endif /* !DARWIN */

#if defined(THREADS)
/* We need to lock around the bitmap update in the write fault handler  */
/* in order to avoid the risk of losing a bit.  We do this with a       */
/* test-and-set spin lock if we know how to do that.  Otherwise we      */
/* check whether we are already in the handler and use the dumb but     */
/* safe fallback algorithm of setting all bits in the word.             */
/* Contention should be very rare, so we do the minimum to handle it    */
/* correctly.                                                           */
#ifdef MK_AO_HAVE_test_and_set_acquire
  MK_GC_INNER volatile MK_AO_TS_t MK_GC_fault_handler_lock = MK_AO_TS_INITIALIZER;
  static void async_set_pht_entry_from_index(volatile page_hash_table db,
                                             size_t index)
  {
    while (MK_AO_test_and_set_acquire(&MK_GC_fault_handler_lock) == MK_AO_TS_SET) {
      /* empty */
    }
    /* Could also revert to set_pht_entry_from_index_safe if initial    */
    /* MK_GC_test_and_set fails.                                           */
    set_pht_entry_from_index(db, index);
    MK_AO_CLEAR(&MK_GC_fault_handler_lock);
  }
#else /* !MK_AO_HAVE_test_and_set_acquire */
# error No test_and_set operation: Introduces a race.
  /* THIS WOULD BE INCORRECT!                                           */
  /* The dirty bit vector may be temporarily wrong,                     */
  /* just before we notice the conflict and correct it. We may end up   */
  /* looking at it while it's wrong.  But this requires contention      */
  /* exactly when a GC is triggered, which seems far less likely to     */
  /* fail than the old code, which had no reported failures.  Thus we   */
  /* leave it this way while we think of something better, or support   */
  /* MK_GC_test_and_set on the remaining platforms.                        */
  static int * volatile currently_updating = 0;
  static void async_set_pht_entry_from_index(volatile page_hash_table db,
                                             size_t index)
  {
    int update_dummy;
    currently_updating = &update_dummy;
    set_pht_entry_from_index(db, index);
    /* If we get contention in the 10 or so instruction window here,    */
    /* and we get stopped by a GC between the two updates, we lose!     */
    if (currently_updating != &update_dummy) {
        set_pht_entry_from_index_safe(db, index);
        /* We claim that if two threads concurrently try to update the  */
        /* dirty bit vector, the first one to execute UPDATE_START      */
        /* will see it changed when UPDATE_END is executed.  (Note that */
        /* &update_dummy must differ in two distinct threads.)  It      */
        /* will then execute set_pht_entry_from_index_safe, thus        */
        /* returning us to a safe state, though not soon enough.        */
    }
  }
#endif /* !MK_AO_HAVE_test_and_set_acquire */
#else /* !THREADS */
# define async_set_pht_entry_from_index(db, index) \
                        set_pht_entry_from_index(db, index)
#endif /* !THREADS */

#ifdef CHECKSUMS
  void MK_GC_record_fault(struct hblk * h); /* from checksums.c */
#endif

#ifndef DARWIN

# if !defined(MSWIN32) && !defined(MSWINCE)
#   include <errno.h>
#   if defined(FREEBSD) || defined(HURD) || defined(HPUX)
#     define SIG_OK (sig == SIGBUS || sig == SIGSEGV)
#   else
#     define SIG_OK (sig == SIGSEGV)
                            /* Catch SIGSEGV but ignore SIGBUS. */
#   endif
#   if defined(FREEBSD)
#     ifndef SEGV_ACCERR
#       define SEGV_ACCERR 2
#     endif
#     if defined(POWERPC)
#       define AIM  /* Pretend that we're AIM. */
#       include <machine/trap.h>
#       define CODE_OK (si -> si_code == EXC_DSI \
                        || si -> si_code == SEGV_ACCERR)
#     else
#       define CODE_OK (si -> si_code == BUS_PAGE_FAULT \
                        || si -> si_code == SEGV_ACCERR)
#     endif
#   elif defined(OSF1)
#     define CODE_OK (si -> si_code == 2 /* experimentally determined */)
#   elif defined(IRIX5)
#     define CODE_OK (si -> si_code == EACCES)
#   elif defined(HURD)
#     define CODE_OK TRUE
#   elif defined(LINUX)
#     define CODE_OK TRUE
      /* Empirically c.trapno == 14, on IA32, but is that useful?       */
      /* Should probably consider alignment issues on other             */
      /* architectures.                                                 */
#   elif defined(HPUX)
#     define CODE_OK (si -> si_code == SEGV_ACCERR \
                      || si -> si_code == BUS_ADRERR \
                      || si -> si_code == BUS_UNKNOWN \
                      || si -> si_code == SEGV_UNKNOWN \
                      || si -> si_code == BUS_OBJERR)
#   elif defined(SUNOS5SIGS)
#     define CODE_OK (si -> si_code == SEGV_ACCERR)
#   endif
#   ifndef NO_GETCONTEXT
#     include <ucontext.h>
#   endif
    STATIC void MK_GC_write_fault_handler(int sig, siginfo_t *si, void *raw_sc)
# else /* defined(MSWIN32) || defined(MSWINCE) */
#   define SIG_OK (exc_info -> ExceptionRecord -> ExceptionCode \
                     == STATUS_ACCESS_VIOLATION)
#   define CODE_OK (exc_info -> ExceptionRecord -> ExceptionInformation[0] \
                      == 1) /* Write fault */
    STATIC LONG WINAPI MK_GC_write_fault_handler(
                                struct _EXCEPTION_POINTERS *exc_info)
# endif /* MSWIN32 || MSWINCE */
  {
#   if !defined(MSWIN32) && !defined(MSWINCE)
        char *addr = si -> si_addr;
#   else
        char * addr = (char *) (exc_info -> ExceptionRecord -> ExceptionInformation[1]);
#   endif
    unsigned i;

    if (SIG_OK && CODE_OK) {
        register struct hblk * h =
                        (struct hblk *)((word)addr & ~(MK_GC_page_size-1));
        MK_GC_bool in_allocd_block;
#       ifdef CHECKSUMS
          MK_GC_record_fault(h);
#       endif

#       ifdef SUNOS5SIGS
            /* Address is only within the correct physical page.        */
            in_allocd_block = FALSE;
            for (i = 0; i < divHBLKSZ(MK_GC_page_size); i++) {
              if (HDR(h+i) != 0) {
                in_allocd_block = TRUE;
                break;
              }
            }
#       else
            in_allocd_block = (HDR(addr) != 0);
#       endif
        if (!in_allocd_block) {
            /* FIXME - We should make sure that we invoke the   */
            /* old handler with the appropriate calling         */
            /* sequence, which often depends on SA_SIGINFO.     */

            /* Heap blocks now begin and end on page boundaries */
#           if defined(MSWIN32) || defined(MSWINCE)
            EXCEPTION_HANDLER_PTR old_handler;
#           else
            struct old_sig_handler old_handler;
#           endif

#           if defined(MSWIN32) || defined(MSWINCE)
            old_handler = MK_GC_old_exception_handler;
#           else

#             if defined(FREEBSD) || defined(HURD) || defined(HPUX)
                if (sig == SIGBUS) {
                   old_handler = MK_GC_old_bus_handler;
                } else
#             endif
                /* else */ {
                  old_handler = MK_GC_old_segv_handler;
                }
#           endif

#           if !defined(MSWIN32) && !defined(MSWINCE)
            if (!old_handler.used_SIGINFO && (old_handler._sa_handler == SIG_DFL))
              ABORT_ARG1("Unexpected bus error or segmentation fault", " at %p", addr);
#           else
            if (old_handler == NULL) return(EXCEPTION_CONTINUE_SEARCH);
#           endif
            else {
                /*
                 * FIXME: This code should probably check if the
                 * old signal handler used the traditional style and
                 * if so call it using that style.
                 */
#               if defined(MSWIN32) || defined(MSWINCE)
                    return((*old_handler)(exc_info));
#               else
                    if (old_handler.used_SIGINFO)
                      (old_handler._sa_sigaction) (sig, si, raw_sc);
                    else
                      /* FIXME: should pass nonstandard args as well. */
                      (old_handler._sa_handler) (sig);
                    return;
#               endif
            }
        }
        UNPROTECT(h, MK_GC_page_size);
        /* We need to make sure that no collection occurs between       */
        /* the UNPROTECT and the setting of the dirty bit.  Otherwise   */
        /* a write by a third thread might go unnoticed.  Reversing     */
        /* the order is just as bad, since we would end up unprotecting */
        /* a page in a GC cycle during which it's not marked.           */
        /* Currently we do this by disabling the thread stopping        */
        /* signals while this handler is running.  An alternative might */
        /* be to record the fact that we're about to unprotect, or      */
        /* have just unprotected a page in the GC's thread structure,   */
        /* and then to have the thread stopping code set the dirty      */
        /* flag, if necessary.                                          */
        for (i = 0; i < divHBLKSZ(MK_GC_page_size); i++) {
            size_t index = PHT_HASH(h+i);

            async_set_pht_entry_from_index(MK_GC_dirty_pages, index);
        }
        /* The write may not take place before dirty bits are read.     */
        /* But then we'll fault again ...                               */
#       if defined(MSWIN32) || defined(MSWINCE)
            return(EXCEPTION_CONTINUE_EXECUTION);
#       else
            return;
#       endif
    }
#   if defined(MSWIN32) || defined(MSWINCE)
      return EXCEPTION_CONTINUE_SEARCH;
#   else
      ABORT_ARG1("Unexpected bus error or segmentation fault",
                 " at %p", addr);
#   endif
  }

# ifdef MK_GC_WIN32_THREADS
    MK_GC_INNER void MK_GC_set_write_fault_handler(void)
    {
      SetUnhandledExceptionFilter(MK_GC_write_fault_handler);
    }
# endif
#endif /* !DARWIN */

/* We hold the allocation lock.  We expect block h to be written        */
/* shortly.  Ensure that all pages containing any part of the n hblks   */
/* starting at h are no longer protected.  If is_ptrfree is false, also */
/* ensure that they will subsequently appear to be dirty.  Not allowed  */
/* to call MK_GC_printf (and the friends) here, see Win32 MK_GC_stop_world()  */
/* for the information.                                                 */
MK_GC_INNER void MK_GC_remove_protection(struct hblk *h, word nblocks,
                                   MK_GC_bool is_ptrfree)
{
    struct hblk * h_trunc;  /* Truncated to page boundary */
    struct hblk * h_end;    /* Page boundary following block end */
    struct hblk * current;

#   if defined(GWW_VDB)
      if (MK_GC_GWW_AVAILABLE()) return;
#   endif
    if (!MK_GC_dirty_maintained) return;
    h_trunc = (struct hblk *)((word)h & ~(MK_GC_page_size-1));
    h_end = (struct hblk *)(((word)(h + nblocks) + MK_GC_page_size-1)
                            & ~(MK_GC_page_size-1));
    if (h_end == h_trunc + 1 &&
        get_pht_entry_from_index(MK_GC_dirty_pages, PHT_HASH(h_trunc))) {
        /* already marked dirty, and hence unprotected. */
        return;
    }
    for (current = h_trunc; (word)current < (word)h_end; ++current) {
        size_t index = PHT_HASH(current);
        if (!is_ptrfree || (word)current < (word)h
            || (word)current >= (word)(h + nblocks)) {
            async_set_pht_entry_from_index(MK_GC_dirty_pages, index);
        }
    }
    UNPROTECT(h_trunc, (ptr_t)h_end - (ptr_t)h_trunc);
}

#if !defined(DARWIN)
  MK_GC_INNER void MK_GC_dirty_init(void)
  {
#   if !defined(MSWIN32) && !defined(MSWINCE)
      struct sigaction act, oldact;
      act.sa_flags = SA_RESTART | SA_SIGINFO;
      act.sa_sigaction = MK_GC_write_fault_handler;
      (void)sigemptyset(&act.sa_mask);
#     if defined(THREADS) && !defined(MK_GC_OPENBSD_UTHREADS) \
         && !defined(MK_GC_WIN32_THREADS) && !defined(NACL)
        /* Arrange to postpone the signal while we are in a write fault */
        /* handler.  This effectively makes the handler atomic w.r.t.   */
        /* stopping the world for GC.                                   */
        (void)sigaddset(&act.sa_mask, MK_GC_get_suspend_signal());
#     endif
#   endif /* !MSWIN32 */
    MK_GC_VERBOSE_LOG_PRINTF(
                "Initializing mprotect virtual dirty bit implementation\n");
    MK_GC_dirty_maintained = TRUE;
    if (MK_GC_page_size % HBLKSIZE != 0) {
        ABORT("Page size not multiple of HBLKSIZE");
    }
#   if !defined(MSWIN32) && !defined(MSWINCE)
      /* act.sa_restorer is deprecated and should not be initialized. */
#     if defined(MK_GC_IRIX_THREADS)
        sigaction(SIGSEGV, 0, &oldact);
        sigaction(SIGSEGV, &act, 0);
#     else
        {
          int res = sigaction(SIGSEGV, &act, &oldact);
          if (res != 0) ABORT("Sigaction failed");
        }
#     endif
      if (oldact.sa_flags & SA_SIGINFO) {
        MK_GC_old_segv_handler._sa_sigaction = oldact.sa_sigaction;
        MK_GC_old_segv_handler.used_SIGINFO = TRUE;
       } else {
        MK_GC_old_segv_handler._sa_handler = oldact.sa_handler;
        MK_GC_old_segv_handler.used_SIGINFO = FALSE;
      }
      if (!MK_GC_old_segv_handler.used_SIGINFO && (MK_GC_old_segv_handler._sa_handler == SIG_IGN)) {
        WARN("Previously ignored segmentation violation!?\n", 0);
        MK_GC_old_segv_handler._sa_handler = SIG_DFL;
      }
      if (!MK_GC_old_segv_handler.used_SIGINFO && (MK_GC_old_segv_handler._sa_handler != SIG_DFL)) {
        MK_GC_VERBOSE_LOG_PRINTF("Replaced other SIGSEGV handler\n");
      }
#   if defined(HPUX) || defined(LINUX) || defined(HURD) \
       || (defined(FREEBSD) && defined(SUNOS5SIGS))
      sigaction(SIGBUS, &act, &oldact);
      if ((oldact.sa_flags & SA_SIGINFO) != 0) {
        MK_GC_old_bus_handler._sa_sigaction = oldact.sa_sigaction;
        MK_GC_old_bus_handler.used_SIGINFO = TRUE;
      } else {
        MK_GC_old_bus_handler._sa_handler = oldact.sa_handler;
        MK_GC_old_bus_handler.used_SIGINFO = FALSE;
      }
      if (!MK_GC_old_bus_handler.used_SIGINFO && (MK_GC_old_bus_handler._sa_handler == SIG_IGN)) {
        WARN("Previously ignored bus error!?\n", 0);
#       if !defined(LINUX)
          MK_GC_old_bus_handler._sa_handler = SIG_DFL;
#       else
          /* MK_GC_old_bus_handler is not used by MK_GC_write_fault_handler.  */
#       endif
      } else if (!MK_GC_old_bus_handler.used_SIGINFO && (MK_GC_old_bus_handler._sa_handler != SIG_DFL)) {
          MK_GC_VERBOSE_LOG_PRINTF("Replaced other SIGBUS handler\n");
      }
#   endif /* HPUX || LINUX || HURD || (FREEBSD && SUNOS5SIGS) */
#   endif /* ! MS windows */
#   if defined(GWW_VDB)
      if (MK_GC_gww_dirty_init())
        return;
#   endif
#   if defined(MSWIN32)
      MK_GC_old_exception_handler = SetUnhandledExceptionFilter(MK_GC_write_fault_handler);
      if (MK_GC_old_exception_handler != NULL) {
        MK_GC_COND_LOG_PRINTF("Replaced other UnhandledExceptionFilter\n");
      }
#   elif defined(MSWINCE)
      /* MPROTECT_VDB is unsupported for WinCE at present.      */
      /* FIXME: implement it (if possible). */
#   endif
  }
#endif /* !DARWIN */

MK_GC_API int MK_GC_CALL MK_GC_incremental_protection_needs(void)
{
    MK_GC_ASSERT(MK_GC_is_initialized);

    if (MK_GC_page_size == HBLKSIZE) {
        return MK_GC_PROTECTS_POINTER_HEAP;
    } else {
        return MK_GC_PROTECTS_POINTER_HEAP | MK_GC_PROTECTS_PTRFREE_HEAP;
    }
}
#define HAVE_INCREMENTAL_PROTECTION_NEEDS

#define IS_PTRFREE(hhdr) ((hhdr)->hb_descr == 0)
#define PAGE_ALIGNED(x) !((word)(x) & (MK_GC_page_size - 1))

STATIC void MK_GC_protect_heap(void)
{
    ptr_t start;
    size_t len;
    struct hblk * current;
    struct hblk * current_start;  /* Start of block to be protected. */
    struct hblk * limit;
    unsigned i;
    MK_GC_bool protect_all =
          (0 != (MK_GC_incremental_protection_needs() & MK_GC_PROTECTS_PTRFREE_HEAP));
    for (i = 0; i < MK_GC_n_heap_sects; i++) {
        start = MK_GC_heap_sects[i].hs_start;
        len = MK_GC_heap_sects[i].hs_bytes;
        if (protect_all) {
          PROTECT(start, len);
        } else {
          MK_GC_ASSERT(PAGE_ALIGNED(len));
          MK_GC_ASSERT(PAGE_ALIGNED(start));
          current_start = current = (struct hblk *)start;
          limit = (struct hblk *)(start + len);
          while ((word)current < (word)limit) {
            hdr * hhdr;
            word nhblks;
            MK_GC_bool is_ptrfree;

            MK_GC_ASSERT(PAGE_ALIGNED(current));
            GET_HDR(current, hhdr);
            if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
              /* This can happen only if we're at the beginning of a    */
              /* heap segment, and a block spans heap segments.         */
              /* We will handle that block as part of the preceding     */
              /* segment.                                               */
              MK_GC_ASSERT(current_start == current);
              current_start = ++current;
              continue;
            }
            if (HBLK_IS_FREE(hhdr)) {
              MK_GC_ASSERT(PAGE_ALIGNED(hhdr -> hb_sz));
              nhblks = divHBLKSZ(hhdr -> hb_sz);
              is_ptrfree = TRUE;        /* dirty on alloc */
            } else {
              nhblks = OBJ_SZ_TO_BLOCKS(hhdr -> hb_sz);
              is_ptrfree = IS_PTRFREE(hhdr);
            }
            if (is_ptrfree) {
              if ((word)current_start < (word)current) {
                PROTECT(current_start, (ptr_t)current - (ptr_t)current_start);
              }
              current_start = (current += nhblks);
            } else {
              current += nhblks;
            }
          }
          if ((word)current_start < (word)current) {
            PROTECT(current_start, (ptr_t)current - (ptr_t)current_start);
          }
        }
    }
}

/* We assume that either the world is stopped or its OK to lose dirty   */
/* bits while this is happening (as in MK_GC_enable_incremental).          */
MK_GC_INNER void MK_GC_read_dirty(void)
{
#   if defined(GWW_VDB)
      if (MK_GC_GWW_AVAILABLE()) {
        MK_GC_gww_read_dirty();
        return;
      }
#   endif
    BCOPY((word *)MK_GC_dirty_pages, MK_GC_grungy_pages,
          (sizeof MK_GC_dirty_pages));
    BZERO((word *)MK_GC_dirty_pages, (sizeof MK_GC_dirty_pages));
    MK_GC_protect_heap();
}

MK_GC_INNER MK_GC_bool MK_GC_page_was_dirty(struct hblk *h)
{
    register word index;

#   if defined(GWW_VDB)
      if (MK_GC_GWW_AVAILABLE())
        return MK_GC_gww_page_was_dirty(h);
#   endif

    index = PHT_HASH(h);
    return(HDR(h) == 0 || get_pht_entry_from_index(MK_GC_grungy_pages, index));
}

/*
 * Acquiring the allocation lock here is dangerous, since this
 * can be called from within MK_GC_call_with_alloc_lock, and the cord
 * package does so.  On systems that allow nested lock acquisition, this
 * happens to work.
 * On other systems, SET_LOCK_HOLDER and friends must be suitably defined.
 */

#if 0
static MK_GC_bool syscall_acquired_lock = FALSE;   /* Protected by GC lock. */

void MK_GC_begin_syscall(void)
{
    /* FIXME: Resurrecting this code would require fixing the   */
    /* test, which can spuriously return TRUE.                  */
    if (!I_HOLD_LOCK()) {
        LOCK();
        syscall_acquired_lock = TRUE;
    }
}

void MK_GC_end_syscall(void)
{
    if (syscall_acquired_lock) {
        syscall_acquired_lock = FALSE;
        UNLOCK();
    }
}

void MK_GC_unprotect_range(ptr_t addr, word len)
{
    struct hblk * start_block;
    struct hblk * end_block;
    register struct hblk *h;
    ptr_t obj_start;

    if (!MK_GC_dirty_maintained) return;
    obj_start = MK_GC_base(addr);
    if (obj_start == 0) return;
    if (MK_GC_base(addr + len - 1) != obj_start) {
        ABORT("MK_GC_unprotect_range(range bigger than object)");
    }
    start_block = (struct hblk *)((word)addr & ~(MK_GC_page_size - 1));
    end_block = (struct hblk *)((word)(addr + len - 1) & ~(MK_GC_page_size - 1));
    end_block += MK_GC_page_size/HBLKSIZE - 1;
    for (h = start_block; (word)h <= (word)end_block; h++) {
        register word index = PHT_HASH(h);

        async_set_pht_entry_from_index(MK_GC_dirty_pages, index);
    }
    UNPROTECT(start_block,
              ((ptr_t)end_block - (ptr_t)start_block) + HBLKSIZE);
}


/* We no longer wrap read by default, since that was causing too many   */
/* problems.  It is preferred that the client instead avoids writing    */
/* to the write-protected heap with a system call.                      */
/* This still serves as sample code if you do want to wrap system calls.*/

#if !defined(MSWIN32) && !defined(MSWINCE) && !defined(MK_GC_USE_LD_WRAP)
/* Replacement for UNIX system call.                                    */
/* Other calls that write to the heap should be handled similarly.      */
/* Note that this doesn't work well for blocking reads:  It will hold   */
/* the allocation lock for the entire duration of the call.             */
/* Multi-threaded clients should really ensure that it won't block,     */
/* either by setting the descriptor non-blocking, or by calling select  */
/* or poll first, to make sure that input is available.                 */
/* Another, preferred alternative is to ensure that system calls never  */
/* write to the protected heap (see above).                             */
# include <unistd.h>
# include <sys/uio.h>
ssize_t read(int fd, void *buf, size_t nbyte)
{
    int result;

    MK_GC_begin_syscall();
    MK_GC_unprotect_range(buf, (word)nbyte);
#   if defined(IRIX5) || defined(MK_GC_LINUX_THREADS)
        /* Indirect system call may not always be easily available.     */
        /* We could call _read, but that would interfere with the       */
        /* libpthread interception of read.                             */
        /* On Linux, we have to be careful with the linuxthreads        */
        /* read interception.                                           */
        {
            struct iovec iov;

            iov.iov_base = buf;
            iov.iov_len = nbyte;
            result = readv(fd, &iov, 1);
        }
#   else
#     if defined(HURD)
        result = __read(fd, buf, nbyte);
#     else
        /* The two zero args at the end of this list are because one
           IA-64 syscall() implementation actually requires six args
           to be passed, even though they aren't always used. */
        result = syscall(SYS_read, fd, buf, nbyte, 0, 0);
#     endif /* !HURD */
#   endif
    MK_GC_end_syscall();
    return(result);
}
#endif /* !MSWIN32 && !MSWINCE && !MK_GC_LINUX_THREADS */

#if defined(MK_GC_USE_LD_WRAP) && !defined(THREADS)
    /* We use the GNU ld call wrapping facility.                        */
    /* I'm not sure that this actually wraps whatever version of read   */
    /* is called by stdio.  That code also mentions __read.             */
#   include <unistd.h>
    ssize_t __wrap_read(int fd, void *buf, size_t nbyte)
    {
        int result;

        MK_GC_begin_syscall();
        MK_GC_unprotect_range(buf, (word)nbyte);
        result = __real_read(fd, buf, nbyte);
        MK_GC_end_syscall();
        return(result);
    }

    /* We should probably also do this for __read, or whatever stdio    */
    /* actually calls.                                                  */
#endif
#endif /* 0 */

# ifdef CHECKSUMS
    MK_GC_INNER MK_GC_bool MK_GC_page_was_ever_dirty(struct hblk * h MK_GC_ATTR_UNUSED)
    {
#     if defined(GWW_VDB)
        if (MK_GC_GWW_AVAILABLE())
          return MK_GC_gww_page_was_ever_dirty(h);
#     endif
      return(TRUE);
    }
# endif /* CHECKSUMS */

#endif /* MPROTECT_VDB */

#ifdef PROC_VDB
/* See DEFAULT_VDB for interface descriptions.  */

/* This implementation assumes a Solaris 2.X like /proc                 */
/* pseudo-file-system from which we can read page modified bits.  This  */
/* facility is far from optimal (e.g. we would like to get the info for */
/* only some of the address space), but it avoids intercepting system   */
/* calls.                                                               */

# include <errno.h>
# include <sys/types.h>
# include <sys/signal.h>
# include <sys/fault.h>
# include <sys/syscall.h>
# include <sys/procfs.h>
# include <sys/stat.h>

# define INITIAL_BUF_SZ 16384
  STATIC word MK_GC_proc_buf_size = INITIAL_BUF_SZ;
  STATIC char *MK_GC_proc_buf = NULL;
  STATIC int MK_GC_proc_fd = 0;

MK_GC_INNER void MK_GC_dirty_init(void)
{
    int fd;
    char buf[30];

    if (MK_GC_bytes_allocd != 0 || MK_GC_bytes_allocd_before_gc != 0) {
      memset(MK_GC_written_pages, 0xff, sizeof(page_hash_table));
      MK_GC_VERBOSE_LOG_PRINTF(
                "Allocated %lu bytes: all pages may have been written\n",
                (unsigned long)(MK_GC_bytes_allocd + MK_GC_bytes_allocd_before_gc));
    }

    (void)snprintf(buf, sizeof(buf), "/proc/%ld", (long)getpid());
    buf[sizeof(buf) - 1] = '\0';
    fd = open(buf, O_RDONLY);
    if (fd < 0) {
        ABORT("/proc open failed");
    }
    MK_GC_proc_fd = syscall(SYS_ioctl, fd, PIOCOPENPD, 0);
    close(fd);
    syscall(SYS_fcntl, MK_GC_proc_fd, F_SETFD, FD_CLOEXEC);
    if (MK_GC_proc_fd < 0) {
        WARN("/proc ioctl(PIOCOPENPD) failed", 0);
        return;
    }

    MK_GC_dirty_maintained = TRUE;
    MK_GC_proc_buf = MK_GC_scratch_alloc(MK_GC_proc_buf_size);
    if (MK_GC_proc_buf == NULL)
      ABORT("Insufficient space for /proc read");
}

# define READ read

MK_GC_INNER void MK_GC_read_dirty(void)
{
    int nmaps;
    unsigned long npages;
    unsigned pagesize;
    ptr_t vaddr, limit;
    struct prasmap * map;
    char * bufp;
    int i;

    BZERO(MK_GC_grungy_pages, sizeof(MK_GC_grungy_pages));
    bufp = MK_GC_proc_buf;
    if (READ(MK_GC_proc_fd, bufp, MK_GC_proc_buf_size) <= 0) {
        /* Retry with larger buffer.    */
        word new_size = 2 * MK_GC_proc_buf_size;
        char *new_buf;

        WARN("/proc read failed: MK_GC_proc_buf_size = %" WARN_PRIdPTR "\n",
             (signed_word)MK_GC_proc_buf_size);
        new_buf = MK_GC_scratch_alloc(new_size);
        if (new_buf != 0) {
            MK_GC_proc_buf = bufp = new_buf;
            MK_GC_proc_buf_size = new_size;
        }
        if (READ(MK_GC_proc_fd, bufp, MK_GC_proc_buf_size) <= 0) {
            WARN("Insufficient space for /proc read\n", 0);
            /* Punt:        */
            memset(MK_GC_grungy_pages, 0xff, sizeof (page_hash_table));
            memset(MK_GC_written_pages, 0xff, sizeof(page_hash_table));
            return;
        }
    }

    /* Copy dirty bits into MK_GC_grungy_pages     */
    nmaps = ((struct prpageheader *)bufp) -> pr_nmap;
#   ifdef DEBUG_DIRTY_BITS
      MK_GC_log_printf("Proc VDB read: pr_nmap= %u, pr_npage= %lu\n",
                    nmaps, ((struct prpageheader *)bufp)->pr_npage);
#   endif
    bufp += sizeof(struct prpageheader);
    for (i = 0; i < nmaps; i++) {
        map = (struct prasmap *)bufp;
        vaddr = (ptr_t)(map -> pr_vaddr);
        npages = map -> pr_npage;
        pagesize = map -> pr_pagesize;
#       ifdef DEBUG_DIRTY_BITS
          MK_GC_log_printf(
                "pr_vaddr= %p, npage= %lu, mflags= 0x%x, pagesize= 0x%x\n",
                vaddr, npages, map->pr_mflags, pagesize);
#       endif

        bufp += sizeof(struct prasmap);
        limit = vaddr + pagesize * npages;
        for (; (word)vaddr < (word)limit; vaddr += pagesize) {
            if ((*bufp++) & PG_MODIFIED) {
                register struct hblk * h;
                ptr_t next_vaddr = vaddr + pagesize;
#               ifdef DEBUG_DIRTY_BITS
                  MK_GC_log_printf("dirty page at: %p\n", vaddr);
#               endif
                for (h = (struct hblk *)vaddr;
                     (word)h < (word)next_vaddr; h++) {
                    register word index = PHT_HASH(h);
                    set_pht_entry_from_index(MK_GC_grungy_pages, index);
                }
            }
        }
        bufp = (char *)(((word)bufp + (sizeof(long)-1)) & ~(sizeof(long)-1));
    }
#   ifdef DEBUG_DIRTY_BITS
      MK_GC_log_printf("Proc VDB read done.\n");
#   endif

    /* Update MK_GC_written_pages. */
    MK_GC_or_pages(MK_GC_written_pages, MK_GC_grungy_pages);
}

# undef READ
#endif /* PROC_VDB */

#ifdef PCR_VDB

# include "vd/PCR_VD.h"

# define NPAGES (32*1024)       /* 128 MB */

PCR_VD_DB MK_GC_grungy_bits[NPAGES];

STATIC ptr_t MK_GC_vd_base = NULL;
                        /* Address corresponding to MK_GC_grungy_bits[0]   */
                        /* HBLKSIZE aligned.                            */

MK_GC_INNER void MK_GC_dirty_init(void)
{
    MK_GC_dirty_maintained = TRUE;
    /* For the time being, we assume the heap generally grows up */
    MK_GC_vd_base = MK_GC_heap_sects[0].hs_start;
    if (MK_GC_vd_base == 0) {
        ABORT("Bad initial heap segment");
    }
    if (PCR_VD_Start(HBLKSIZE, MK_GC_vd_base, NPAGES*HBLKSIZE)
        != PCR_ERes_okay) {
        ABORT("Dirty bit initialization failed");
    }
}

MK_GC_INNER void MK_GC_read_dirty(void)
{
    /* lazily enable dirty bits on newly added heap sects */
    {
        static int onhs = 0;
        int nhs = MK_GC_n_heap_sects;
        for(; onhs < nhs; onhs++) {
            PCR_VD_WriteProtectEnable(
                    MK_GC_heap_sects[onhs].hs_start,
                    MK_GC_heap_sects[onhs].hs_bytes );
        }
    }

    if (PCR_VD_Clear(MK_GC_vd_base, NPAGES*HBLKSIZE, MK_GC_grungy_bits)
        != PCR_ERes_okay) {
        ABORT("Dirty bit read failed");
    }
}

MK_GC_INNER MK_GC_bool MK_GC_page_was_dirty(struct hblk *h)
{
    if ((word)h < (word)MK_GC_vd_base
        || (word)h >= (word)(MK_GC_vd_base + NPAGES*HBLKSIZE)) {
      return(TRUE);
    }
    return(MK_GC_grungy_bits[h - (struct hblk *)MK_GC_vd_base] & PCR_VD_DB_dirtyBit);
}

MK_GC_INNER void MK_GC_remove_protection(struct hblk *h, word nblocks,
                                   MK_GC_bool is_ptrfree MK_GC_ATTR_UNUSED)
{
    PCR_VD_WriteProtectDisable(h, nblocks*HBLKSIZE);
    PCR_VD_WriteProtectEnable(h, nblocks*HBLKSIZE);
}

#endif /* PCR_VDB */

#if defined(MPROTECT_VDB) && defined(DARWIN)
/* The following sources were used as a "reference" for this exception
   handling code:
      1. Apple's mach/xnu documentation
      2. Timothy J. Wood's "Mach Exception Handlers 101" post to the
         omnigroup's macosx-dev list.
         www.omnigroup.com/mailman/archive/macosx-dev/2000-June/014178.html
      3. macosx-nat.c from Apple's GDB source code.
*/

/* The bug that caused all this trouble should now be fixed. This should
   eventually be removed if all goes well. */

/* #define BROKEN_EXCEPTION_HANDLING */

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/thread_status.h>
#include <mach/exception.h>
#include <mach/task.h>
#include <pthread.h>

/* These are not defined in any header, although they are documented */
extern boolean_t
exc_server(mach_msg_header_t *, mach_msg_header_t *);

extern kern_return_t
exception_raise(mach_port_t, mach_port_t, mach_port_t, exception_type_t,
                exception_data_t, mach_msg_type_number_t);

extern kern_return_t
exception_raise_state(mach_port_t, mach_port_t, mach_port_t, exception_type_t,
                      exception_data_t, mach_msg_type_number_t,
                      thread_state_flavor_t*, thread_state_t,
                      mach_msg_type_number_t, thread_state_t,
                      mach_msg_type_number_t*);

extern kern_return_t
exception_raise_state_identity(mach_port_t, mach_port_t, mach_port_t,
                               exception_type_t, exception_data_t,
                               mach_msg_type_number_t, thread_state_flavor_t*,
                               thread_state_t, mach_msg_type_number_t,
                               thread_state_t, mach_msg_type_number_t*);

MK_GC_API_OSCALL kern_return_t
catch_exception_raise(mach_port_t exception_port, mach_port_t thread,
                      mach_port_t task, exception_type_t exception,
                      exception_data_t code, mach_msg_type_number_t code_count);

/* These should never be called, but just in case...  */
MK_GC_API_OSCALL kern_return_t
catch_exception_raise_state(mach_port_name_t exception_port MK_GC_ATTR_UNUSED,
    int exception MK_GC_ATTR_UNUSED, exception_data_t code MK_GC_ATTR_UNUSED,
    mach_msg_type_number_t codeCnt MK_GC_ATTR_UNUSED, int flavor MK_GC_ATTR_UNUSED,
    thread_state_t old_state MK_GC_ATTR_UNUSED, int old_stateCnt MK_GC_ATTR_UNUSED,
    thread_state_t new_state MK_GC_ATTR_UNUSED, int new_stateCnt MK_GC_ATTR_UNUSED)
{
  ABORT_RET("Unexpected catch_exception_raise_state invocation");
  return(KERN_INVALID_ARGUMENT);
}

MK_GC_API_OSCALL kern_return_t
catch_exception_raise_state_identity(
    mach_port_name_t exception_port MK_GC_ATTR_UNUSED,
    mach_port_t thread MK_GC_ATTR_UNUSED, mach_port_t task MK_GC_ATTR_UNUSED,
    int exception MK_GC_ATTR_UNUSED, exception_data_t code MK_GC_ATTR_UNUSED,
    mach_msg_type_number_t codeCnt MK_GC_ATTR_UNUSED, int flavor MK_GC_ATTR_UNUSED,
    thread_state_t old_state MK_GC_ATTR_UNUSED, int old_stateCnt MK_GC_ATTR_UNUSED,
    thread_state_t new_state MK_GC_ATTR_UNUSED, int new_stateCnt MK_GC_ATTR_UNUSED)
{
  ABORT_RET("Unexpected catch_exception_raise_state_identity invocation");
  return(KERN_INVALID_ARGUMENT);
}

#define MAX_EXCEPTION_PORTS 16

static struct {
  mach_msg_type_number_t count;
  exception_mask_t      masks[MAX_EXCEPTION_PORTS];
  exception_handler_t   ports[MAX_EXCEPTION_PORTS];
  exception_behavior_t  behaviors[MAX_EXCEPTION_PORTS];
  thread_state_flavor_t flavors[MAX_EXCEPTION_PORTS];
} MK_GC_old_exc_ports;

STATIC struct {
  void (*volatile os_callback[3])(void);
  mach_port_t exception;
# if defined(THREADS)
    mach_port_t reply;
# endif
} MK_GC_ports = {
  {
    /* This is to prevent stripping these routines as dead.     */
    (void (*)(void))catch_exception_raise,
    (void (*)(void))catch_exception_raise_state,
    (void (*)(void))catch_exception_raise_state_identity
  },
# ifdef THREADS
    0, /* for 'exception' */
# endif
  0
};

typedef struct {
    mach_msg_header_t head;
} MK_GC_msg_t;

typedef enum {
    MK_GC_MP_NORMAL,
    MK_GC_MP_DISCARDING,
    MK_GC_MP_STOPPED
} MK_GC_mprotect_state_t;

#ifdef THREADS
  /* FIXME: 1 and 2 seem to be safe to use in the msgh_id field, but it */
  /* is not documented.  Use the source and see if they should be OK.   */
# define ID_STOP 1
# define ID_RESUME 2

  /* This value is only used on the reply port. */
# define ID_ACK 3

  STATIC MK_GC_mprotect_state_t MK_GC_mprotect_state = 0;

  /* The following should ONLY be called when the world is stopped.     */
  STATIC void MK_GC_mprotect_thread_notify(mach_msg_id_t id)
  {
    struct {
      MK_GC_msg_t msg;
      mach_msg_trailer_t trailer;
    } buf;
    mach_msg_return_t r;

    /* remote, local */
    buf.msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    buf.msg.head.msgh_size = sizeof(buf.msg);
    buf.msg.head.msgh_remote_port = MK_GC_ports.exception;
    buf.msg.head.msgh_local_port = MACH_PORT_NULL;
    buf.msg.head.msgh_id = id;

    r = mach_msg(&buf.msg.head, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_LARGE,
                 sizeof(buf.msg), sizeof(buf), MK_GC_ports.reply,
                 MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (r != MACH_MSG_SUCCESS)
      ABORT("mach_msg failed in MK_GC_mprotect_thread_notify");
    if (buf.msg.head.msgh_id != ID_ACK)
      ABORT("Invalid ack in MK_GC_mprotect_thread_notify");
  }

  /* Should only be called by the mprotect thread */
  STATIC void MK_GC_mprotect_thread_reply(void)
  {
    MK_GC_msg_t msg;
    mach_msg_return_t r;
    /* remote, local */

    msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg.head.msgh_size = sizeof(msg);
    msg.head.msgh_remote_port = MK_GC_ports.reply;
    msg.head.msgh_local_port = MACH_PORT_NULL;
    msg.head.msgh_id = ID_ACK;

    r = mach_msg(&msg.head, MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL,
                 MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (r != MACH_MSG_SUCCESS)
      ABORT("mach_msg failed in MK_GC_mprotect_thread_reply");
  }

  MK_GC_INNER void MK_GC_mprotect_stop(void)
  {
    MK_GC_mprotect_thread_notify(ID_STOP);
  }

  MK_GC_INNER void MK_GC_mprotect_resume(void)
  {
    MK_GC_mprotect_thread_notify(ID_RESUME);
  }

# ifndef MK_GC_NO_THREADS_DISCOVERY
    MK_GC_INNER void MK_GC_darwin_register_mach_handler_thread(mach_port_t thread);
# endif

#else
  /* The compiler should optimize away any MK_GC_mprotect_state computations */
# define MK_GC_mprotect_state MK_GC_MP_NORMAL
#endif /* !THREADS */

STATIC void *MK_GC_mprotect_thread(void *arg)
{
  mach_msg_return_t r;
  /* These two structures contain some private kernel data.  We don't   */
  /* need to access any of it so we don't bother defining a proper      */
  /* struct.  The correct definitions are in the xnu source code.       */
  struct {
    mach_msg_header_t head;
    char data[256];
  } reply;
  struct {
    mach_msg_header_t head;
    mach_msg_body_t msgh_body;
    char data[1024];
  } msg;
  mach_msg_id_t id;

  if ((word)arg == (word)-1) return 0; /* to make compiler happy */

# if defined(THREADS) && !defined(MK_GC_NO_THREADS_DISCOVERY)
    MK_GC_darwin_register_mach_handler_thread(mach_thread_self());
# endif

  for(;;) {
    r = mach_msg(&msg.head, MACH_RCV_MSG | MACH_RCV_LARGE |
                 (MK_GC_mprotect_state == MK_GC_MP_DISCARDING ? MACH_RCV_TIMEOUT : 0),
                 0, sizeof(msg), MK_GC_ports.exception,
                 MK_GC_mprotect_state == MK_GC_MP_DISCARDING ? 0
                 : MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    id = r == MACH_MSG_SUCCESS ? msg.head.msgh_id : -1;

#   if defined(THREADS)
      if(MK_GC_mprotect_state == MK_GC_MP_DISCARDING) {
        if(r == MACH_RCV_TIMED_OUT) {
          MK_GC_mprotect_state = MK_GC_MP_STOPPED;
          MK_GC_mprotect_thread_reply();
          continue;
        }
        if(r == MACH_MSG_SUCCESS && (id == ID_STOP || id == ID_RESUME))
          ABORT("Out of order mprotect thread request");
      }
#   endif /* THREADS */

    if (r != MACH_MSG_SUCCESS) {
      ABORT_ARG2("mach_msg failed",
                 ": errcode= %d (%s)", (int)r, mach_error_string(r));
    }

    switch(id) {
#     if defined(THREADS)
        case ID_STOP:
          if(MK_GC_mprotect_state != MK_GC_MP_NORMAL)
            ABORT("Called mprotect_stop when state wasn't normal");
          MK_GC_mprotect_state = MK_GC_MP_DISCARDING;
          break;
        case ID_RESUME:
          if(MK_GC_mprotect_state != MK_GC_MP_STOPPED)
            ABORT("Called mprotect_resume when state wasn't stopped");
          MK_GC_mprotect_state = MK_GC_MP_NORMAL;
          MK_GC_mprotect_thread_reply();
          break;
#     endif /* THREADS */
        default:
          /* Handle the message (calls catch_exception_raise) */
          if(!exc_server(&msg.head, &reply.head))
            ABORT("exc_server failed");
          /* Send the reply */
          r = mach_msg(&reply.head, MACH_SEND_MSG, reply.head.msgh_size, 0,
                       MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                       MACH_PORT_NULL);
          if(r != MACH_MSG_SUCCESS) {
            /* This will fail if the thread dies, but the thread */
            /* shouldn't die... */
#           ifdef BROKEN_EXCEPTION_HANDLING
              MK_GC_err_printf("mach_msg failed with %d %s while sending "
                            "exc reply\n", (int)r, mach_error_string(r));
#           else
              ABORT("mach_msg failed while sending exception reply");
#           endif
          }
    } /* switch */
  } /* for(;;) */
}

/* All this SIGBUS code shouldn't be necessary. All protection faults should
   be going through the mach exception handler. However, it seems a SIGBUS is
   occasionally sent for some unknown reason. Even more odd, it seems to be
   meaningless and safe to ignore. */
#ifdef BROKEN_EXCEPTION_HANDLING

  /* Updates to this aren't atomic, but the SIGBUS'es seem pretty rare.    */
  /* Even if this doesn't get updated property, it isn't really a problem. */
  STATIC int MK_GC_sigbus_count = 0;

  STATIC void MK_GC_darwin_sigbus(int num, siginfo_t *sip, void *context)
  {
    if (num != SIGBUS)
      ABORT("Got a non-sigbus signal in the sigbus handler");

    /* Ugh... some seem safe to ignore, but too many in a row probably means
       trouble. MK_GC_sigbus_count is reset for each mach exception that is
       handled */
    if (MK_GC_sigbus_count >= 8) {
      ABORT("Got more than 8 SIGBUSs in a row!");
    } else {
      MK_GC_sigbus_count++;
      WARN("Ignoring SIGBUS.\n", 0);
    }
  }
#endif /* BROKEN_EXCEPTION_HANDLING */

MK_GC_INNER void MK_GC_dirty_init(void)
{
  kern_return_t r;
  mach_port_t me;
  pthread_t thread;
  pthread_attr_t attr;
  exception_mask_t mask;

# ifdef CAN_HANDLE_FORK
    if (MK_GC_handle_fork) {
      /* To both support GC incremental mode and GC functions usage in  */
      /* the forked child, pthread_atfork should be used to install     */
      /* handlers that switch off MK_GC_dirty_maintained in the child      */
      /* gracefully (unprotecting all pages and clearing                */
      /* MK_GC_mach_handler_thread).  For now, we just disable incremental */
      /* mode if fork() handling is requested by the client.            */
      MK_GC_COND_LOG_PRINTF("GC incremental mode disabled since fork()"
                         " handling requested\n");
      return;
    }
# endif

  MK_GC_VERBOSE_LOG_PRINTF("Initializing mach/darwin mprotect"
                        " virtual dirty bit implementation\n");
# ifdef BROKEN_EXCEPTION_HANDLING
    WARN("Enabling workarounds for various darwin "
         "exception handling bugs.\n", 0);
# endif
  MK_GC_dirty_maintained = TRUE;
  if (MK_GC_page_size % HBLKSIZE != 0) {
    ABORT("Page size not multiple of HBLKSIZE");
  }

  MK_GC_task_self = me = mach_task_self();

  r = mach_port_allocate(me, MACH_PORT_RIGHT_RECEIVE, &MK_GC_ports.exception);
  if (r != KERN_SUCCESS)
    ABORT("mach_port_allocate failed (exception port)");

  r = mach_port_insert_right(me, MK_GC_ports.exception, MK_GC_ports.exception,
                             MACH_MSG_TYPE_MAKE_SEND);
  if (r != KERN_SUCCESS)
    ABORT("mach_port_insert_right failed (exception port)");

#  if defined(THREADS)
     r = mach_port_allocate(me, MACH_PORT_RIGHT_RECEIVE, &MK_GC_ports.reply);
     if(r != KERN_SUCCESS)
       ABORT("mach_port_allocate failed (reply port)");
#  endif

  /* The exceptions we want to catch */
  mask = EXC_MASK_BAD_ACCESS;

  r = task_get_exception_ports(me, mask, MK_GC_old_exc_ports.masks,
                               &MK_GC_old_exc_ports.count, MK_GC_old_exc_ports.ports,
                               MK_GC_old_exc_ports.behaviors,
                               MK_GC_old_exc_ports.flavors);
  if (r != KERN_SUCCESS)
    ABORT("task_get_exception_ports failed");

  r = task_set_exception_ports(me, mask, MK_GC_ports.exception, EXCEPTION_DEFAULT,
                               MK_GC_MACH_THREAD_STATE);
  if (r != KERN_SUCCESS)
    ABORT("task_set_exception_ports failed");
  if (pthread_attr_init(&attr) != 0)
    ABORT("pthread_attr_init failed");
  if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
    ABORT("pthread_attr_setdetachedstate failed");

# undef pthread_create
  /* This will call the real pthread function, not our wrapper */
  if (pthread_create(&thread, &attr, MK_GC_mprotect_thread, NULL) != 0)
    ABORT("pthread_create failed");
  pthread_attr_destroy(&attr);

  /* Setup the sigbus handler for ignoring the meaningless SIGBUSs */
# ifdef BROKEN_EXCEPTION_HANDLING
    {
      struct sigaction sa, oldsa;
      sa.sa_handler = (SIG_HNDLR_PTR)MK_GC_darwin_sigbus;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART|SA_SIGINFO;
      /* sa.sa_restorer is deprecated and should not be initialized. */
      if (sigaction(SIGBUS, &sa, &oldsa) < 0)
        ABORT("sigaction failed");
      if ((SIG_HNDLR_PTR)oldsa.sa_handler != SIG_DFL) {
        MK_GC_VERBOSE_LOG_PRINTF("Replaced other SIGBUS handler\n");
      }
    }
# endif /* BROKEN_EXCEPTION_HANDLING  */
}

/* The source code for Apple's GDB was used as a reference for the      */
/* exception forwarding code.  This code is similar to be GDB code only */
/* because there is only one way to do it.                              */
STATIC kern_return_t MK_GC_forward_exception(mach_port_t thread, mach_port_t task,
                                          exception_type_t exception,
                                          exception_data_t data,
                                          mach_msg_type_number_t data_count)
{
  unsigned int i;
  kern_return_t r;
  mach_port_t port;
  exception_behavior_t behavior;
  thread_state_flavor_t flavor;

  thread_state_data_t thread_state;
  mach_msg_type_number_t thread_state_count = THREAD_STATE_MAX;

  for (i=0; i < MK_GC_old_exc_ports.count; i++)
    if (MK_GC_old_exc_ports.masks[i] & (1 << exception))
      break;
  if (i == MK_GC_old_exc_ports.count)
    ABORT("No handler for exception!");

  port = MK_GC_old_exc_ports.ports[i];
  behavior = MK_GC_old_exc_ports.behaviors[i];
  flavor = MK_GC_old_exc_ports.flavors[i];

  if (behavior == EXCEPTION_STATE || behavior == EXCEPTION_STATE_IDENTITY) {
    r = thread_get_state(thread, flavor, thread_state, &thread_state_count);
    if(r != KERN_SUCCESS)
      ABORT("thread_get_state failed in forward_exception");
    }

  switch(behavior) {
    case EXCEPTION_STATE:
      r = exception_raise_state(port, thread, task, exception, data, data_count,
                                &flavor, thread_state, thread_state_count,
                                thread_state, &thread_state_count);
      break;
    case EXCEPTION_STATE_IDENTITY:
      r = exception_raise_state_identity(port, thread, task, exception, data,
                                         data_count, &flavor, thread_state,
                                         thread_state_count, thread_state,
                                         &thread_state_count);
      break;
    /* case EXCEPTION_DEFAULT: */ /* default signal handlers */
    default: /* user-supplied signal handlers */
      r = exception_raise(port, thread, task, exception, data, data_count);
  }

  if (behavior == EXCEPTION_STATE || behavior == EXCEPTION_STATE_IDENTITY) {
    r = thread_set_state(thread, flavor, thread_state, thread_state_count);
    if (r != KERN_SUCCESS)
      ABORT("thread_set_state failed in forward_exception");
  }
  return r;
}

#define FWD() MK_GC_forward_exception(thread, task, exception, code, code_count)

#ifdef ARM32
# define DARWIN_EXC_STATE         ARM_EXCEPTION_STATE
# define DARWIN_EXC_STATE_COUNT   ARM_EXCEPTION_STATE_COUNT
# define DARWIN_EXC_STATE_T       arm_exception_state_t
# define DARWIN_EXC_STATE_DAR     THREAD_FLD(far)
#elif defined(POWERPC)
# if CPP_WORDSZ == 32
#   define DARWIN_EXC_STATE       PPC_EXCEPTION_STATE
#   define DARWIN_EXC_STATE_COUNT PPC_EXCEPTION_STATE_COUNT
#   define DARWIN_EXC_STATE_T     ppc_exception_state_t
# else
#   define DARWIN_EXC_STATE       PPC_EXCEPTION_STATE64
#   define DARWIN_EXC_STATE_COUNT PPC_EXCEPTION_STATE64_COUNT
#   define DARWIN_EXC_STATE_T     ppc_exception_state64_t
# endif
# define DARWIN_EXC_STATE_DAR     THREAD_FLD(dar)
#elif defined(I386) || defined(X86_64)
# if CPP_WORDSZ == 32
#   if defined(i386_EXCEPTION_STATE_COUNT) \
       && !defined(x86_EXCEPTION_STATE32_COUNT)
      /* Use old naming convention for 32-bit x86.      */
#     define DARWIN_EXC_STATE           i386_EXCEPTION_STATE
#     define DARWIN_EXC_STATE_COUNT     i386_EXCEPTION_STATE_COUNT
#     define DARWIN_EXC_STATE_T         i386_exception_state_t
#   else
#     define DARWIN_EXC_STATE           x86_EXCEPTION_STATE32
#     define DARWIN_EXC_STATE_COUNT     x86_EXCEPTION_STATE32_COUNT
#     define DARWIN_EXC_STATE_T         x86_exception_state32_t
#   endif
# else
#   define DARWIN_EXC_STATE       x86_EXCEPTION_STATE64
#   define DARWIN_EXC_STATE_COUNT x86_EXCEPTION_STATE64_COUNT
#   define DARWIN_EXC_STATE_T     x86_exception_state64_t
# endif
# define DARWIN_EXC_STATE_DAR     THREAD_FLD(faultvaddr)
#else
# error FIXME for non-arm/ppc/x86 darwin
#endif

/* This violates the namespace rules but there isn't anything that can  */
/* be done about it.  The exception handling stuff is hard coded to     */
/* call this.  catch_exception_raise, catch_exception_raise_state and   */
/* and catch_exception_raise_state_identity are called from OS.         */
MK_GC_API_OSCALL kern_return_t
catch_exception_raise(mach_port_t exception_port MK_GC_ATTR_UNUSED,
                      mach_port_t thread, mach_port_t task MK_GC_ATTR_UNUSED,
                      exception_type_t exception, exception_data_t code,
                      mach_msg_type_number_t code_count MK_GC_ATTR_UNUSED)
{
  kern_return_t r;
  char *addr;
  struct hblk *h;
  unsigned int i;
  thread_state_flavor_t flavor = DARWIN_EXC_STATE;
  mach_msg_type_number_t exc_state_count = DARWIN_EXC_STATE_COUNT;
  DARWIN_EXC_STATE_T exc_state;

  if (exception != EXC_BAD_ACCESS || code[0] != KERN_PROTECTION_FAILURE) {
#   ifdef DEBUG_EXCEPTION_HANDLING
      /* We aren't interested, pass it on to the old handler */
      MK_GC_log_printf("Exception: 0x%x Code: 0x%x 0x%x in catch...\n",
                    exception, code_count > 0 ? code[0] : -1,
                    code_count > 1 ? code[1] : -1);
#   endif
    return FWD();
  }

  r = thread_get_state(thread, flavor, (natural_t*)&exc_state,
                       &exc_state_count);
  if(r != KERN_SUCCESS) {
    /* The thread is supposed to be suspended while the exception       */
    /* handler is called.  This shouldn't fail.                         */
#   ifdef BROKEN_EXCEPTION_HANDLING
      MK_GC_err_printf("thread_get_state failed in catch_exception_raise\n");
      return KERN_SUCCESS;
#   else
      ABORT("thread_get_state failed in catch_exception_raise");
#   endif
  }

  /* This is the address that caused the fault */
  addr = (char*) exc_state.DARWIN_EXC_STATE_DAR;
  if (HDR(addr) == 0) {
    /* Ugh... just like the SIGBUS problem above, it seems we get       */
    /* a bogus KERN_PROTECTION_FAILURE every once and a while.  We wait */
    /* till we get a bunch in a row before doing anything about it.     */
    /* If a "real" fault ever occurs it'll just keep faulting over and  */
    /* over and we'll hit the limit pretty quickly.                     */
#   ifdef BROKEN_EXCEPTION_HANDLING
      static char *last_fault;
      static int last_fault_count;

      if(addr != last_fault) {
        last_fault = addr;
        last_fault_count = 0;
      }
      if(++last_fault_count < 32) {
        if(last_fault_count == 1)
          WARN("Ignoring KERN_PROTECTION_FAILURE at %p\n", addr);
        return KERN_SUCCESS;
      }

      MK_GC_err_printf(
        "Unexpected KERN_PROTECTION_FAILURE at %p; aborting...\n", addr);
      /* Can't pass it along to the signal handler because that is      */
      /* ignoring SIGBUS signals.  We also shouldn't call ABORT here as */
      /* signals don't always work too well from the exception handler. */
      EXIT();
#   else /* BROKEN_EXCEPTION_HANDLING */
      /* Pass it along to the next exception handler
         (which should call SIGBUS/SIGSEGV) */
      return FWD();
#   endif /* !BROKEN_EXCEPTION_HANDLING */
  }

# ifdef BROKEN_EXCEPTION_HANDLING
    /* Reset the number of consecutive SIGBUSs */
    MK_GC_sigbus_count = 0;
# endif

  if (MK_GC_mprotect_state == MK_GC_MP_NORMAL) { /* common case */
    h = (struct hblk*)((word)addr & ~(MK_GC_page_size-1));
    UNPROTECT(h, MK_GC_page_size);
    for (i = 0; i < divHBLKSZ(MK_GC_page_size); i++) {
      register int index = PHT_HASH(h+i);
      async_set_pht_entry_from_index(MK_GC_dirty_pages, index);
    }
  } else if (MK_GC_mprotect_state == MK_GC_MP_DISCARDING) {
    /* Lie to the thread for now. No sense UNPROTECT()ing the memory
       when we're just going to PROTECT() it again later. The thread
       will just fault again once it resumes */
  } else {
    /* Shouldn't happen, i don't think */
    MK_GC_err_printf("KERN_PROTECTION_FAILURE while world is stopped\n");
    return FWD();
  }
  return KERN_SUCCESS;
}
#undef FWD

#ifndef NO_DESC_CATCH_EXCEPTION_RAISE
  /* These symbols should have REFERENCED_DYNAMICALLY (0x10) bit set to */
  /* let strip know they are not to be stripped.                        */
  __asm__(".desc _catch_exception_raise, 0x10");
  __asm__(".desc _catch_exception_raise_state, 0x10");
  __asm__(".desc _catch_exception_raise_state_identity, 0x10");
#endif

#endif /* DARWIN && MPROTECT_VDB */

#ifndef HAVE_INCREMENTAL_PROTECTION_NEEDS
  MK_GC_API int MK_GC_CALL MK_GC_incremental_protection_needs(void)
  {
    return MK_GC_PROTECTS_NONE;
  }
#endif /* !HAVE_INCREMENTAL_PROTECTION_NEEDS */

#ifdef ECOS
  /* Undo sbrk() redirection. */
# undef sbrk
#endif

/* If value is non-zero then allocate executable memory.        */
MK_GC_API void MK_GC_CALL MK_GC_set_pages_executable(int value)
{
  MK_GC_ASSERT(!MK_GC_is_initialized);
  /* Even if IGNORE_PAGES_EXECUTABLE is defined, MK_GC_pages_executable is */
  /* touched here to prevent a compiler warning.                        */
  MK_GC_pages_executable = (MK_GC_bool)(value != 0);
}

/* Returns non-zero if the GC-allocated memory is executable.   */
/* MK_GC_get_pages_executable is defined after all the places      */
/* where MK_GC_get_pages_executable is undefined.                  */
MK_GC_API int MK_GC_CALL MK_GC_get_pages_executable(void)
{
# ifdef IGNORE_PAGES_EXECUTABLE
    return 1;   /* Always allocate executable memory. */
# else
    return (int)MK_GC_pages_executable;
# endif
}

/* Call stack save code for debugging.  Should probably be in           */
/* mach_dep.c, but that requires reorganization.                        */

/* I suspect the following works for most X86 *nix variants, so         */
/* long as the frame pointer is explicitly stored.  In the case of gcc, */
/* compiler flags (e.g. -fomit-frame-pointer) determine whether it is.  */
#if defined(I386) && defined(LINUX) && defined(SAVE_CALL_CHAIN)
#   include <features.h>

    struct frame {
        struct frame *fr_savfp;
        long    fr_savpc;
        long    fr_arg[NARGS];  /* All the arguments go here.   */
    };
#endif

#if defined(SPARC)
#  if defined(LINUX)
#    include <features.h>

     struct frame {
        long    fr_local[8];
        long    fr_arg[6];
        struct frame *fr_savfp;
        long    fr_savpc;
#       ifndef __arch64__
          char  *fr_stret;
#       endif
        long    fr_argd[6];
        long    fr_argx[0];
     };
#  elif defined (DRSNX)
#    include <sys/sparc/frame.h>
#  elif defined(OPENBSD)
#    include <frame.h>
#  elif defined(FREEBSD) || defined(NETBSD)
#    include <machine/frame.h>
#  else
#    include <sys/frame.h>
#  endif
#  if NARGS > 6
#    error We only know how to get the first 6 arguments
#  endif
#endif /* SPARC */

#ifdef NEED_CALLINFO
/* Fill in the pc and argument information for up to NFRAMES of my      */
/* callers.  Ignore my frame and my callers frame.                      */

#ifdef LINUX
#   include <unistd.h>
#endif

#endif /* NEED_CALLINFO */

#if defined(MK_GC_HAVE_BUILTIN_BACKTRACE)
# ifdef _MSC_VER
#  include "private/msvc_dbg.h"
# else
#  include <execinfo.h>
# endif
#endif

#ifdef SAVE_CALL_CHAIN

#if NARGS == 0 && NFRAMES % 2 == 0 /* No padding */ \
    && defined(MK_GC_HAVE_BUILTIN_BACKTRACE)

#ifdef REDIRECT_MALLOC
  /* Deal with possible malloc calls in backtrace by omitting   */
  /* the infinitely recursing backtrace.                        */
# ifdef THREADS
    __thread    /* If your compiler doesn't understand this */
                /* you could use something like pthread_getspecific.    */
# endif
  MK_GC_in_save_callers = FALSE;
#endif

MK_GC_INNER void MK_GC_save_callers(struct callinfo info[NFRAMES])
{
  void * tmp_info[NFRAMES + 1];
  int npcs, i;
# define IGNORE_FRAMES 1

  /* We retrieve NFRAMES+1 pc values, but discard the first, since it   */
  /* points to our own frame.                                           */
# ifdef REDIRECT_MALLOC
    if (MK_GC_in_save_callers) {
      info[0].ci_pc = (word)(&MK_GC_save_callers);
      for (i = 1; i < NFRAMES; ++i) info[i].ci_pc = 0;
      return;
    }
    MK_GC_in_save_callers = TRUE;
# endif
  MK_GC_STATIC_ASSERT(sizeof(struct callinfo) == sizeof(void *));
  npcs = backtrace((void **)tmp_info, NFRAMES + IGNORE_FRAMES);
  BCOPY(tmp_info+IGNORE_FRAMES, info, (npcs - IGNORE_FRAMES) * sizeof(void *));
  for (i = npcs - IGNORE_FRAMES; i < NFRAMES; ++i) info[i].ci_pc = 0;
# ifdef REDIRECT_MALLOC
    MK_GC_in_save_callers = FALSE;
# endif
}

#else /* No builtin backtrace; do it ourselves */

#if (defined(OPENBSD) || defined(NETBSD) || defined(FREEBSD)) && defined(SPARC)
#  define FR_SAVFP fr_fp
#  define FR_SAVPC fr_pc
#else
#  define FR_SAVFP fr_savfp
#  define FR_SAVPC fr_savpc
#endif

#if defined(SPARC) && (defined(__arch64__) || defined(__sparcv9))
#   define BIAS 2047
#else
#   define BIAS 0
#endif

MK_GC_INNER void MK_GC_save_callers(struct callinfo info[NFRAMES])
{
  struct frame *frame;
  struct frame *fp;
  int nframes = 0;
# ifdef I386
    /* We assume this is turned on only with gcc as the compiler. */
    asm("movl %%ebp,%0" : "=r"(frame));
    fp = frame;
# else
    frame = (struct frame *)MK_GC_save_regs_in_stack();
    fp = (struct frame *)((long) frame -> FR_SAVFP + BIAS);
#endif

   for (; !((word)fp HOTTER_THAN (word)frame)
          && !((word)MK_GC_stackbottom HOTTER_THAN (word)fp)
          && nframes < NFRAMES;
       fp = (struct frame *)((long) fp -> FR_SAVFP + BIAS), nframes++) {
      register int i;

      info[nframes].ci_pc = fp->FR_SAVPC;
#     if NARGS > 0
        for (i = 0; i < NARGS; i++) {
          info[nframes].ci_arg[i] = ~(fp->fr_arg[i]);
        }
#     endif /* NARGS > 0 */
  }
  if (nframes < NFRAMES) info[nframes].ci_pc = 0;
}

#endif /* No builtin backtrace */

#endif /* SAVE_CALL_CHAIN */

#ifdef NEED_CALLINFO

/* Print info to stderr.  We do NOT hold the allocation lock */
MK_GC_INNER void MK_GC_print_callers(struct callinfo info[NFRAMES])
{
    int i;
    static int reentry_count = 0;
    MK_GC_bool stop = FALSE;
    DCL_LOCK_STATE;

    /* FIXME: This should probably use a different lock, so that we     */
    /* become callable with or without the allocation lock.             */
    LOCK();
      ++reentry_count;
    UNLOCK();

#   if NFRAMES == 1
      MK_GC_err_printf("\tCaller at allocation:\n");
#   else
      MK_GC_err_printf("\tCall chain at allocation:\n");
#   endif
    for (i = 0; i < NFRAMES && !stop; i++) {
        if (info[i].ci_pc == 0) break;
#       if NARGS > 0
        {
          int j;

          MK_GC_err_printf("\t\targs: ");
          for (j = 0; j < NARGS; j++) {
            if (j != 0) MK_GC_err_printf(", ");
            MK_GC_err_printf("%d (0x%X)", ~(info[i].ci_arg[j]),
                                        ~(info[i].ci_arg[j]));
          }
          MK_GC_err_printf("\n");
        }
#       endif
        if (reentry_count > 1) {
            /* We were called during an allocation during       */
            /* a previous MK_GC_print_callers call; punt.          */
            MK_GC_err_printf("\t\t##PC##= 0x%lx\n", info[i].ci_pc);
            continue;
        }
        {
#         if defined(MK_GC_HAVE_BUILTIN_BACKTRACE) \
             && !defined(MK_GC_BACKTRACE_SYMBOLS_BROKEN)
            char **sym_name =
              backtrace_symbols((void **)(&(info[i].ci_pc)), 1);
            char *name = sym_name[0];
#         else
            char buf[40];
            char *name = buf;
            (void)snprintf(buf, sizeof(buf), "##PC##= 0x%lx", info[i].ci_pc);
            buf[sizeof(buf) - 1] = '\0';
#         endif
#         if defined(LINUX) && !defined(SMALL_CONFIG)
            /* Try for a line number. */
            {
                FILE *pipe;
#               define EXE_SZ 100
                static char exe_name[EXE_SZ];
#               define CMD_SZ 200
                char cmd_buf[CMD_SZ];
#               define RESULT_SZ 200
                static char result_buf[RESULT_SZ];
                size_t result_len;
                char *old_preload;
#               define PRELOAD_SZ 200
                char preload_buf[PRELOAD_SZ];
                static MK_GC_bool found_exe_name = FALSE;
                static MK_GC_bool will_fail = FALSE;
                int ret_code;
                /* Try to get it via a hairy and expensive scheme.      */
                /* First we get the name of the executable:             */
                if (will_fail) goto out;
                if (!found_exe_name) {
                  ret_code = readlink("/proc/self/exe", exe_name, EXE_SZ);
                  if (ret_code < 0 || ret_code >= EXE_SZ
                      || exe_name[0] != '/') {
                    will_fail = TRUE;   /* Don't try again. */
                    goto out;
                  }
                  exe_name[ret_code] = '\0';
                  found_exe_name = TRUE;
                }
                /* Then we use popen to start addr2line -e <exe> <addr> */
                /* There are faster ways to do this, but hopefully this */
                /* isn't time critical.                                 */
                (void)snprintf(cmd_buf, sizeof(cmd_buf),
                               "/usr/bin/addr2line -f -e %s 0x%lx",
                               exe_name, (unsigned long)info[i].ci_pc);
                cmd_buf[sizeof(cmd_buf) - 1] = '\0';
                old_preload = GETENV("LD_PRELOAD");
                if (0 != old_preload) {
                  size_t old_len = strlen(old_preload);
                  if (old_len >= PRELOAD_SZ) {
                    will_fail = TRUE;
                    goto out;
                  }
                  BCOPY(old_preload, preload_buf, old_len + 1);
                  unsetenv ("LD_PRELOAD");
                }
                pipe = popen(cmd_buf, "r");
                if (0 != old_preload
                    && 0 != setenv ("LD_PRELOAD", preload_buf, 0)) {
                  WARN("Failed to reset LD_PRELOAD\n", 0);
                }
                if (pipe == NULL
                    || (result_len = fread(result_buf, 1,
                                           RESULT_SZ - 1, pipe)) == 0) {
                  if (pipe != NULL) pclose(pipe);
                  will_fail = TRUE;
                  goto out;
                }
                if (result_buf[result_len - 1] == '\n') --result_len;
                result_buf[result_len] = 0;
                if (result_buf[0] == '?'
                    || (result_buf[result_len-2] == ':'
                        && result_buf[result_len-1] == '0')) {
                    pclose(pipe);
                    goto out;
                }
                /* Get rid of embedded newline, if any.  Test for "main" */
                {
                   char * nl = strchr(result_buf, '\n');
                   if (nl != NULL
                       && (word)nl < (word)(result_buf + result_len)) {
                     *nl = ':';
                   }
                   if (strncmp(result_buf, "main", nl - result_buf) == 0) {
                     stop = TRUE;
                   }
                }
                if (result_len < RESULT_SZ - 25) {
                  /* Add in hex address */
                  (void)snprintf(&result_buf[result_len],
                                 sizeof(result_buf) - result_len,
                                 " [0x%lx]", (unsigned long)info[i].ci_pc);
                  result_buf[sizeof(result_buf) - 1] = '\0';
                }
                name = result_buf;
                pclose(pipe);
                out:;
            }
#         endif /* LINUX */
          MK_GC_err_printf("\t\t%s\n", name);
#         if defined(MK_GC_HAVE_BUILTIN_BACKTRACE) \
             && !defined(MK_GC_BACKTRACE_SYMBOLS_BROKEN)
            free(sym_name);  /* May call MK_GC_free; that's OK */
#         endif
        }
    }
    LOCK();
      --reentry_count;
    UNLOCK();
}

#endif /* NEED_CALLINFO */

#if defined(LINUX) && defined(__ELF__) && !defined(SMALL_CONFIG)
  /* Dump /proc/self/maps to MK_GC_stderr, to enable looking up names for  */
  /* addresses in FIND_LEAK output.                                     */
  void MK_GC_print_address_map(void)
  {
    char *maps;

    MK_GC_err_printf("---------- Begin address map ----------\n");
    maps = MK_GC_get_maps();
    MK_GC_err_puts(maps != NULL ? maps : "Failed to get map!\n");
    MK_GC_err_printf("---------- End address map ----------\n");
  }
#endif /* LINUX && ELF */
