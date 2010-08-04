/* License: GPLv3 */

#ifndef _XOPEN_SOURCE /* POSIX_MADV_RANDOM */
#define _XOPEN_SOURCE 600
#endif
#ifndef _ATFILE_SOURCE /* fstatat(), openat() */
#define _ATFILE_SOURCE
#endif
#ifndef _GNU_SOURCE /* enable O_CLOEXEC on GNU systems */
#define _GNU_SOURCE 1
#endif

#include "mcdb.h"
#include "nointr.h"
#include "uint32.h"
#include "code_attributes.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#ifdef _THREAD_SAFE
#include <pthread.h>       /* pthread_mutex_t, pthread_mutex_{lock,unlock}() */
static pthread_mutex_t mcdb_global_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
#define pthread_mutex_lock(mutexp) 0
#define pthread_mutex_unlock(mutexp) (void)0
#endif

#ifndef O_CLOEXEC /* O_CLOEXEC available since Linux 2.6.23 */
#define O_CLOEXEC 0
#endif


/* Note: tagc of 0 ('\0') is reserved to indicate no tag */

bool
mcdb_findtagstart(struct mcdb * const restrict m,
                  const char * const restrict key, const size_t klen,
                  const unsigned char tagc)
{
    const uint32_t khash_init = /* init hash value; hash tagc if tagc not 0 */
      (tagc != 0)
        ? uint32_hash_djb_uchar(UINT32_HASH_DJB_INIT, tagc)
        : UINT32_HASH_DJB_INIT;
    const uint32_t khash = uint32_hash_djb(khash_init, key, klen);
    const unsigned char * restrict ptr;

    (void) mcdb_thread_refresh_self(m);
    /* (ignore rc; continue with previous map in case of failure) */

    ptr = m->map->ptr + ((khash << 3) & MCDB_HEADER_MASK) + 4;
    m->hslots = uint32_strunpack_bigendian_aligned_macro(ptr);
    if (!m->hslots)
        return false;
    m->hpos  = uint32_strunpack_bigendian_aligned_macro(ptr-4);
    m->kpos  = m->hpos + (((khash >> 8) % m->hslots) << 3);
    m->khash = khash;
    m->loop  = 0;
    return true;
}

bool
mcdb_findtagnext(struct mcdb * const restrict m,
                 const char * const restrict key, const size_t klen,
                 const unsigned char tagc)
{
    const unsigned char * ptr;
    const unsigned char * const restrict mptr = m->map->ptr;
    uint32_t vpos, khash, len;

    while (m->loop < m->hslots) {
        ptr = mptr + m->kpos + 4;
        vpos = uint32_strunpack_bigendian_aligned_macro(ptr);
        if (!vpos)
            return false;
        khash = uint32_strunpack_bigendian_aligned_macro(ptr-4);
        m->kpos += 8;
        if (m->kpos == m->hpos + (m->hslots << 3))
            m->kpos = m->hpos;
        m->loop += 1;
        if (khash == m->khash) {
            ptr = mptr + vpos;
            len = uint32_strunpack_bigendian_macro(ptr);
            if (tagc != 0
                ? len == klen+1 && tagc == ptr[8] && memcmp(key,ptr+9,klen) == 0
                : len == klen && memcmp(key,ptr+8,klen) == 0) {
                m->dlen = uint32_strunpack_bigendian_macro(ptr+4);
                m->dpos = vpos + 8 + len;
                return true;
            }
        }
    }
    return false;
}

/* read value from mmap const db into buffer and return pointer to buffer
 * (return NULL if position (offset) or length to read will be out-of-bounds)
 * Note: caller must terminate with '\0' if desired, i.e. buf[len] = '\0';
 */
void *
mcdb_read(struct mcdb * const restrict m, const uint32_t pos,
          const uint32_t len, void * const restrict buf)
{
    const uint32_t mapsz = m->map->size;
    return (pos <= mapsz && mapsz - pos >= len) /* bounds check */
      ? memcpy(buf, m->map->ptr + pos, len)
      : NULL;
}



/* Note: __attribute_noinline__ is used to mark less frequent code paths
 * to prevent inlining of seldoms used paths, hopefully improving instruction
 * cache hits.
 */


static void  inline
mcdb_mmap_unmap(struct mcdb_mmap * const restrict map)
  __attribute_nonnull__;

static void  inline
mcdb_mmap_unmap(struct mcdb_mmap * const restrict map)
{
    if (map->ptr)
        munmap(map->ptr, map->size);
    map->ptr   = NULL;
    map->size  = 0;    /* map->size initialization required for mcdb_read() */
}

bool  __attribute_noinline__
mcdb_mmap_init(struct mcdb_mmap * const restrict map, int fd)
{
    struct stat st;
    void * restrict x;

    mcdb_mmap_unmap(map);

    /* size of mcdb is limited to 4 GB minus difference needed to page-align */
    if (fstat(fd, &st) != 0) return false;
    x = mmap(0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (x == MAP_FAILED) return false;
  #ifdef __GNUC__
    __builtin_prefetch((char *)x+960, 0, 1); /*(touch mem page w/ mcdb header)*/
  #endif
    if (st.st_size > USHRT_MAX) /*(skip extra syscall overhead for small mcdb)*/
        posix_madvise(((char *)x)+USHRT_MAX, st.st_size-USHRT_MAX,
                      POSIX_MADV_RANDOM);
    map->ptr   = (unsigned char *)x;
    map->size  = st.st_size;
    map->mtime = st.st_mtime;
    map->next  = NULL;
    map->refcnt= 0;
    return true;
}

void  __attribute_noinline__
mcdb_mmap_free(struct mcdb_mmap * const restrict map)
{
    mcdb_mmap_unmap(map);
    if (map->fn_free) {
        if (map->fname != NULL && map->fname != map->fnamebuf) {
            map->fn_free(map->fname);
            map->fname = NULL;
        }
        map->fn_free(map);
    }
}

void  __attribute_noinline__
mcdb_mmap_destroy(struct mcdb_mmap * const restrict map)
{
    if (map->dfd != -1)
        (void) nointr_close(map->dfd);
    mcdb_mmap_free(map);
}

bool  __attribute_noinline__
mcdb_mmap_reopen(struct mcdb_mmap * const restrict map)
{
    int fd;
    bool rc;

    const int oflags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
  #if defined(__linux) || defined(__sun)
    if (map->dfd != -1) {
        if ((fd = nointr_openat(map->dfd, map->fname, oflags, 0)) == -1)
            return false;
    }
    else
  #endif
    if ((fd = nointr_open(map->fname, oflags, 0)) == -1)
        return false;

    rc = mcdb_mmap_init(map, fd);

    (void) nointr_close(fd); /* close fd once it has been mmap'ed */

    return rc;
}

bool
mcdb_mmap_refresh_check(const struct mcdb_mmap * const restrict map)
{
    struct stat st;
    return (map->ptr == NULL
            || ( (
                  #if defined(__linux) || defined(__sun)
                    map->dfd != -1
                      ? fstatat(map->dfd, map->fname, &st, 0) == 0
                      :
                  #endif
                        stat(map->fname, &st) == 0 )
                && map->mtime != st.st_mtime ) );
}

/*
 * Example use of mcdb_mmap_create() and mcdb_mmap_destroy():
 *   mcdb_mmap_create()
 *   repeat:
 *     (optional) mcdb_mmap_refresh())
 *     (lookups)
 *   mcdb_mmap_destroy();
 *
 * Example use in threaded program where multiple threads access mcdb_mmap:
 *   maintenance thread: mcdb_mmap_create(...)
 *
 *   maintenance thread: mcdb_mmap_refresh_threadsafe(&map) (periodic/triggered)
 *   querying threads:   mcdb_thread_register(m)
 *   querying threads:   mcdb_find(m,key,klen)          (repeat for may lookups)
 *   querying threads:   mcdb_thread_unregister(m)
 *
 *   maintenance thread: mcdb_mmap_destroy(map)
 *
 * Querying threads can register once and then may do many lookups at will.
 * (no need to register, query, unregister each time)
 * Each query (mcdb_findtagstart()) checks for updated mmap by calling
 * mcdb_thread_refresh_self().  If a thread might go for a long period of time
 * without querying, and mcdb_mmap has been updated, then thread can optionally
 * (periodically) call mcdb_thread_refresh_self() to release the outdated mmap
 * before the thread gets around (some time in the future) to its next query.
 *
 * Note: using map->dfd means that if a directory replaces existing directory,
 * mcdb_mmap will not notice.  Pass dname == NULL to skip using map->dfd.
 *
 * Note: use return value from mcdb_mmap_create().  If an error occurs during
 * mcdb_mmap_create(), fn_free(map) is called, whether or not map or NULL was
 * passed as first argument to mcdb_mmap_create().
 */
struct mcdb_mmap *  __attribute_noinline__
mcdb_mmap_create(struct mcdb_mmap * restrict map,
                 const char * const dname  __attribute_unused__,
                 const char * const fname,
                 void * (*fn_malloc)(size_t), void (*fn_free)(void *))
{
    const size_t flen = strlen(fname);
    char *fbuf;

    if (fn_malloc == NULL)
        return NULL;
    if (map == NULL && (map = fn_malloc(sizeof(struct mcdb_mmap))) == NULL)
        return NULL;
    /* initialize */
    memset(map, '\0', sizeof(struct mcdb_mmap));
    map->fn_malloc = fn_malloc;
    map->fn_free   = fn_free;
    map->dfd       = -1;

  #if defined(__linux) || defined(__sun)
    if (dname != NULL) {
        /* caller must have open STDIN, STDOUT, STDERR */
        map->dfd = nointr_open(dname, O_RDONLY|O_CLOEXEC, 0);
        if (map->dfd > STDERR_FILENO) {
            if (O_CLOEXEC == 0)
                (void) fcntl(map->dfd, F_SETFD, FD_CLOEXEC);
        }
        else {
            mcdb_mmap_destroy(map);
            return NULL;
        }
    }
  #else
    if (dname != NULL) {
        const size_t dlen = strlen(dname);
        if (sizeof(map->fnamebuf) >= dlen+flen+2)
            fbuf = map->fnamebuf;
        else if ((fbuf = fn_malloc(dlen+flen+2) == NULL)) {
            mcdb_mmap_destroy(map);
            return NULL;
        }
        memcpy(fbuf, dname, dlen);
        fbuf[dlen] = '/';
        memcpy(fbuf+dlen+1, fname, flen+1);
    }
    else
  #endif
    {
        if (sizeof(map->fnamebuf) > flen)
            fbuf = map->fnamebuf;
        else if ((fbuf = fn_malloc(flen+1)) == NULL) {
            mcdb_mmap_destroy(map);
            return NULL;
        }
        memcpy(fbuf, fname, flen+1);
    }
    map->fname = fbuf;

    if (mcdb_mmap_reopen(map)) {
        ++map->refcnt;
        return map;
    }
    else {
        mcdb_mmap_destroy(map);
        return NULL;
    }
}

bool  __attribute_noinline__
mcdb_mmap_thread_registration(struct mcdb_mmap ** const restrict mapptr,
                              const enum mcdb_flags flags)
{
    struct mcdb_mmap *map;
    struct mcdb_mmap *next = NULL;
    const bool register_use_incr = ((flags & MCDB_REGISTER_USE_INCR) != 0);
    #define register_use_decr (!register_use_incr)

    if (!(flags & MCDB_REGISTER_MUTEX_UNLOCK_HOLD)
        && pthread_mutex_lock(&mcdb_global_mutex) != 0)
        return false;

    /*map = *mapptr;*/  /*(ought to be preceded by StoreLoad memory barrier)*/
    map = *(struct mcdb_mmap * volatile * restrict)mapptr;
    if (map == NULL || (map->ptr == NULL && register_use_incr)) {
        if (!(flags & MCDB_REGISTER_MUTEX_LOCK_HOLD))
            pthread_mutex_unlock(&mcdb_global_mutex);
        return !register_use_incr; /* succeed if unregister; fail if register */
        /* If registering, possibly detected race condition in which another
         * thread released final reference and mcdb was munmap()'d while current
         * thread waited for lock.  It is now invalid to attempt to register use
         * of a resource that has been released.  Caller can detect and reopen*/
    }

    if (register_use_incr) {
        if ((next = map->next) == NULL)
            ++map->refcnt;
        else {
            while (next->next != NULL)
                next = next->next;
            ++next->refcnt;
            *mapptr = next;
        }
    }

    if ((register_use_decr || next != NULL) && --map->refcnt == 0) {
        /* (above handles refcnt decremented to zero and refcnt already zero) */
        while ((next = map->next) != NULL && next->refcnt == 0) {
            map->next = next->next;
            next->fname = NULL;   /* do not free(next->fname) yet */
            mcdb_mmap_free(next);
        }
        if (!(flags & MCDB_REGISTER_MUNMAP_SKIP)) {
            map->fname = NULL;    /* do not free(map->fname) yet */
            mcdb_mmap_free(map);
            if (register_use_decr)
                *mapptr = NULL;
        }
    }
    #undef register_use_decr

    if (!(flags & MCDB_REGISTER_MUTEX_LOCK_HOLD))
        pthread_mutex_unlock(&mcdb_global_mutex);

    return true;
}

/* theaded programs (in while multiple threads are using same struct mcdb_mmap)
 * must reopen and register (update refcnt on previous and new mcdb_mmap) while
 * holding a lock, or else there are race conditions with refcnt'ing. */
bool  __attribute_noinline__
mcdb_mmap_reopen_threadsafe(struct mcdb_mmap ** const restrict mapptr)
{
    bool rc = true;

    if (pthread_mutex_lock(&mcdb_global_mutex) != 0)
        return false;

    if ((*mapptr)->next == NULL) {
        const struct mcdb_mmap * const map = *mapptr;
        struct mcdb_mmap *next;
        if (map->fn_malloc != NULL  /*(else caller misconfigured mcdb_mmap)*/
            && (next = map->fn_malloc(sizeof(struct mcdb_mmap))) != NULL) {
            memcpy(next, map, sizeof(struct mcdb_mmap));
            next->ptr = NULL;       /*(skip munmap() in mcdb_mmap_reopen())*/
            if (map->fname == map->fnamebuf)
                next->fname = next->fnamebuf;
            if ((rc = mcdb_mmap_reopen(next)))
                (*mapptr)->next = next;
            else
                map->fn_free(next);
        }
        else
            rc = false; /* map->fn_malloc failed */
    }
    /* else rc = true;  (map->next already updated e.g. while obtaining lock) */

    if (rc) {
        const enum mcdb_flags mcdb_flags_hold_lock =
            MCDB_REGISTER_USE_INCR
          | MCDB_REGISTER_MUTEX_UNLOCK_HOLD
          | MCDB_REGISTER_MUTEX_LOCK_HOLD;
        rc = mcdb_mmap_thread_registration(mapptr, mcdb_flags_hold_lock);
    }

    pthread_mutex_unlock(&mcdb_global_mutex);
    return rc;
}
