/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

// FIXME: config.h?
#include <stdint.h>
#include <stdbool.h>
// end FIXME
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "extstore.h"

// TODO: better if an init option turns this on/off.
#ifdef EXTSTORE_DEBUG
#define E_DEBUG(...) \
    do { \
        fprintf(stderr, __VA_ARGS__); \
    } while (0)
#else
#define E_DEBUG(...)
#endif

#define STAT_L(e) pthread_mutex_lock(&e->stats_mutex);
#define STAT_UL(e) pthread_mutex_unlock(&e->stats_mutex);
#define STAT_INCR(e, stat, amount) { \
    pthread_mutex_lock(&e->stats_mutex); \
    e->stats.stat += amount; \
    pthread_mutex_unlock(&e->stats_mutex); \
}

#define STAT_DECR(e, stat, amount) { \
    pthread_mutex_lock(&e->stats_mutex); \
    e->stats.stat -= amount; \
    pthread_mutex_unlock(&e->stats_mutex); \
}

typedef struct __store_wbuf {
    struct __store_wbuf *next;
    char *buf;
    char *buf_pos;
    unsigned int free;
    unsigned int size;
    unsigned int offset; /* offset into page this write starts at */
    bool full; /* done writing to this page */
    bool flushed; /* whether wbuf has been flushed to disk */
} _store_wbuf;

typedef struct _store_page {
    pthread_mutex_t mutex; /* Need to be held for most operations */
    uint64_t obj_count; /* _delete can decrease post-closing */
    uint64_t bytes_used; /* _delete can decrease post-closing */
    uint64_t offset; /* starting address of page within fd */
    unsigned int version;
    unsigned int refcount;
    unsigned int allocated;
    unsigned int written; /* item offsets can be past written if wbuf not flushed */
    unsigned int bucket; /* which bucket the page is linked into */
    int fd;
    unsigned short id;
    bool active; /* actively being written to */
    bool closed; /* closed and draining before free */
    bool free; /* on freelist */
    _store_wbuf *wbuf; /* currently active wbuf from the stack */
    struct _store_page *next;
} store_page;

typedef struct store_engine store_engine;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    obj_io *queue;
    store_engine *e;
} store_io_thread;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    store_engine *e;
} store_maint_thread;

/* TODO: Array of FDs for JBOD support */
struct store_engine {
    pthread_mutex_t mutex; /* covers internal stacks and variables */
    store_page *pages; /* directly addressable page list */
    _store_wbuf *wbuf_stack; /* wbuf freelist */
    obj_io *io_stack; /* IO's to use with submitting wbuf's */
    store_io_thread *io_threads;
    store_maint_thread *maint_thread;
    store_page *page_freelist;
    store_page **page_buckets; /* stack of pages currently allocated to each bucket */
    size_t page_size;
    unsigned int version; /* global version counter */
    unsigned int last_io_thread; /* round robin the IO threads */
    unsigned int io_threadcount; /* count of IO threads */
    unsigned int page_count;
    unsigned int page_free; /* unallocated pages */
    unsigned int page_bucketcount; /* count of potential page buckets */
    unsigned int io_depth; /* FIXME: Might cache into thr struct */
    pthread_mutex_t stats_mutex;
    struct extstore_stats stats;
};

static _store_wbuf *wbuf_new(size_t size) {
    _store_wbuf *b = calloc(1, sizeof(_store_wbuf));
    if (b == NULL)
        return NULL;
    b->buf = malloc(size);
    if (b->buf == NULL) {
        free(b);
        return NULL;
    }
    b->buf_pos = b->buf;
    b->free = size;
    b->size = size;
    return b;
}

static store_io_thread *_get_io_thread(store_engine *e) {
    int tid;
    pthread_mutex_lock(&e->mutex);
    tid = (e->last_io_thread + 1) % e->io_threadcount;
    e->last_io_thread = tid;
    pthread_mutex_unlock(&e->mutex);

    return &e->io_threads[tid];
}

static uint64_t _next_version(store_engine *e) {
    return e->version++;
}

static void *extstore_io_thread(void *arg);
static void *extstore_maint_thread(void *arg);

/* Copies stats internal to engine and computes any derived values */
void extstore_get_stats(void *ptr, struct extstore_stats *st) {
    store_engine *e = (store_engine *)ptr;
    STAT_L(e);
    memcpy(st, &e->stats, sizeof(struct extstore_stats));
    STAT_UL(e);

    // grab pages_free/pages_used
    pthread_mutex_lock(&e->mutex);
    st->pages_free = e->page_free;
    st->pages_used = e->page_count - e->page_free;
    pthread_mutex_unlock(&e->mutex);
    // calculate bytes_fragmented.
    // note that open and yet-filled pages count against fragmentation.
    st->bytes_fragmented = st->pages_used * e->page_size -
        st->bytes_used;
}

void extstore_get_page_data(void *ptr, struct extstore_stats *st) {
    store_engine *e = (store_engine *)ptr;
    STAT_L(e);
    memcpy(st->page_data, e->stats.page_data,
            sizeof(struct extstore_page_data) * e->page_count);
    STAT_UL(e);
}

/* TODO: debug mode with prints? error code? */
// TODO: Somehow pass real error codes from config failures
void *extstore_init(char *fn, struct extstore_conf *cf) {
    int i;
    int fd;
    uint64_t offset = 0;
    pthread_t thread;

    if (cf->page_size % cf->wbuf_size != 0) {
        E_DEBUG("EXTSTORE: page_size must be divisible by wbuf_size\n");
        return NULL;
    }
    // Should ensure at least one write buffer per potential page
    if (cf->page_buckets > cf->wbuf_count) {
        E_DEBUG("EXTSTORE: wbuf_count must be >= page_buckets\n");
        return NULL;
    }
    if (cf->page_buckets < 1) {
        E_DEBUG("EXTSTORE: page_buckets must be > 0\n");
        return NULL;
    }

    // TODO: More intelligence around alignment of flash erasure block sizes
    if (cf->page_size % (1024 * 1024 * 2) != 0 ||
        cf->wbuf_size % (1024 * 1024 * 2) != 0) {
        E_DEBUG("EXTSTORE: page_size and wbuf_size must be divisible by 1024*1024*2\n");
        return NULL;
    }

    store_engine *e = calloc(1, sizeof(store_engine));
    if (e == NULL) {
        E_DEBUG("EXTSTORE: failed calloc for engine\n");
        return NULL;
    }

    e->page_size = cf->page_size;
    fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        E_DEBUG("EXTSTORE: failed to open file: %s\n", fn);
#ifdef EXTSTORE_DEBUG
        perror("open");
#endif
        free(e);
        return NULL;
    }

    e->pages = calloc(cf->page_count, sizeof(store_page));
    if (e->pages == NULL) {
        E_DEBUG("EXTSTORE: failed to calloc storage pages\n");
        close(fd);
        free(e);
        return NULL;
    }

    for (i = 0; i < cf->page_count; i++) {
        pthread_mutex_init(&e->pages[i].mutex, NULL);
        e->pages[i].id = i;
        e->pages[i].fd = fd;
        e->pages[i].offset = offset;
        e->pages[i].free = true;
        offset += e->page_size;
    }

    for (i = cf->page_count-1; i > 0; i--) {
        e->pages[i].next = e->page_freelist;
        e->page_freelist = &e->pages[i];
        e->page_free++;
    }

    // 0 is magic "page is freed" version
    e->version = 1;

    e->page_count = cf->page_count;
    // scratch data for stats. TODO: malloc failure handle
    e->stats.page_data =
        calloc(e->page_count, sizeof(struct extstore_page_data));
    e->stats.page_count = e->page_count;
    e->stats.page_size = e->page_size;

    // page buckets lazily have pages assigned into them
    e->page_buckets = calloc(cf->page_buckets, sizeof(store_page *));
    e->page_bucketcount = cf->page_buckets;

    // allocate write buffers
    // also IO's to use for shipping to IO thread
    for (i = 0; i < cf->wbuf_count; i++) {
        _store_wbuf *w = wbuf_new(cf->wbuf_size);
        obj_io *io = calloc(1, sizeof(obj_io));
        /* TODO: on error, loop again and free stack. */
        w->next = e->wbuf_stack;
        e->wbuf_stack = w;
        io->next = e->io_stack;
        e->io_stack = io;
    }

    pthread_mutex_init(&e->mutex, NULL);
    pthread_mutex_init(&e->stats_mutex, NULL);

    e->io_depth = cf->io_depth;

    // spawn threads
    e->io_threads = calloc(cf->io_threadcount, sizeof(store_io_thread));
    for (i = 0; i < cf->io_threadcount; i++) {
        pthread_mutex_init(&e->io_threads[i].mutex, NULL);
        pthread_cond_init(&e->io_threads[i].cond, NULL);
        e->io_threads[i].e = e;
        // FIXME: error handling
        pthread_create(&thread, NULL, extstore_io_thread, &e->io_threads[i]);
    }
    e->io_threadcount = cf->io_threadcount;

    e->maint_thread = calloc(1, sizeof(store_maint_thread));
    e->maint_thread->e = e;
    // FIXME: error handling
    pthread_create(&thread, NULL, extstore_maint_thread, e->maint_thread);

    return (void *)e;
}

void extstore_run_maint(void *ptr) {
    store_engine *e = (store_engine *)ptr;
    pthread_cond_signal(&e->maint_thread->cond);
}

// call with *e locked
static store_page *_allocate_page(store_engine *e, unsigned int bucket) {
    assert(!e->page_buckets[bucket] || e->page_buckets[bucket]->allocated == e->page_size);
    store_page *tmp = e->page_freelist;
    E_DEBUG("EXTSTORE: allocating new page\n");
    if (e->page_free > 0) {
        assert(e->page_freelist != NULL);
        e->page_freelist = tmp->next;
        tmp->next = e->page_buckets[bucket];
        e->page_buckets[bucket] = tmp;
        tmp->active = true;
        tmp->free = false;
        tmp->closed = false;
        tmp->version = _next_version(e);
        tmp->bucket = bucket;
        e->page_free--;
        STAT_INCR(e, page_allocs, 1);
    } else {
        extstore_run_maint(e);
    }
    if (tmp)
        E_DEBUG("EXTSTORE: got page %u\n", tmp->id);
    return tmp;
}

// call with *p locked. locks *e
static void _allocate_wbuf(store_engine *e, store_page *p) {
    _store_wbuf *wbuf = NULL;
    assert(!p->wbuf);
    pthread_mutex_lock(&e->mutex);
    if (e->wbuf_stack) {
        wbuf = e->wbuf_stack;
        e->wbuf_stack = wbuf->next;
        wbuf->next = 0;
    }
    pthread_mutex_unlock(&e->mutex);
    if (wbuf) {
        wbuf->offset = p->allocated;
        p->allocated += wbuf->size;
        wbuf->free = wbuf->size;
        wbuf->buf_pos = wbuf->buf;
        wbuf->full = false;
        wbuf->flushed = false;

        p->wbuf = wbuf;
    }
}

/* callback after wbuf is flushed. can only remove wbuf's from the head onward
 * if successfully flushed, which complicates this routine. each callback
 * attempts to free the wbuf stack, which is finally done when the head wbuf's
 * callback happens.
 * It's rare flushes would happen out of order.
 */
static void _wbuf_cb(void *ep, obj_io *io, int ret) {
    store_engine *e = (store_engine *)ep;
    store_page *p = &e->pages[io->page_id];
    _store_wbuf *w = (_store_wbuf *) io->data;

    // TODO: Examine return code. Not entirely sure how to handle errors.
    // Naive first-pass should probably cause the page to close/free.
    w->flushed = true;
    pthread_mutex_lock(&p->mutex);
    assert(p->wbuf != NULL && p->wbuf == w);
    assert(p->written == w->offset);
    p->written += w->size;
    p->wbuf = NULL;

    if (p->written == e->page_size)
        p->active = false;

    // return the wbuf
    pthread_mutex_lock(&e->mutex);
    w->next = e->wbuf_stack;
    e->wbuf_stack = w;
    // also return the IO we just used.
    io->next = e->io_stack;
    e->io_stack = io;
    pthread_mutex_unlock(&e->mutex);
    pthread_mutex_unlock(&p->mutex);
}

/* Wraps pages current wbuf in an io and submits to IO thread.
 * Called with p locked, locks e.
 */
static void _submit_wbuf(store_engine *e, store_page *p) {
    _store_wbuf *w;
    pthread_mutex_lock(&e->mutex);
    obj_io *io = e->io_stack;
    e->io_stack = io->next;
    pthread_mutex_unlock(&e->mutex);
    w = p->wbuf;

    // zero out the end of the wbuf to allow blind readback of data.
    memset(w->buf + (w->size - w->free), 0, w->free);

    io->next = NULL;
    io->mode = OBJ_IO_WRITE;
    io->page_id = p->id;
    io->data = w;
    io->offset = w->offset;
    io->len = w->size;
    io->buf = w->buf;
    io->cb = _wbuf_cb;

    extstore_submit(e, io);
}

/* engine write function; takes engine, item_io.
 * fast fail if no available write buffer (flushing)
 * lock engine context, find active page, unlock
 * if page full, submit page/buffer to io thread.
 *
 * write is designed to be flaky; if page full, caller must try again to get
 * new page. best if used from a background thread that can harmlessly retry.
 */

int extstore_write_request(void *ptr, unsigned int bucket, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_page *p;
    int ret = -1;
    if (bucket >= e->page_bucketcount)
        return ret;

    pthread_mutex_lock(&e->mutex);
    p = e->page_buckets[bucket];
    if (!p) {
        p = _allocate_page(e, bucket);
    }
    pthread_mutex_unlock(&e->mutex);
    if (!p)
        return ret;

    pthread_mutex_lock(&p->mutex);

    // FIXME: can't null out page_buckets!!!
    // page is full, clear bucket and retry later.
    if (!p->active ||
            ((!p->wbuf || p->wbuf->full) && p->allocated >= e->page_size)) {
        pthread_mutex_unlock(&p->mutex);
        pthread_mutex_lock(&e->mutex);
        _allocate_page(e, bucket);
        pthread_mutex_unlock(&e->mutex);
        return ret;
    }

    // if io won't fit, submit IO for wbuf and find new one.
    if (p->wbuf && p->wbuf->free < io->len && !p->wbuf->full) {
        _submit_wbuf(e, p);
        p->wbuf->full = true;
    }

    if (!p->wbuf && p->allocated < e->page_size) {
        _allocate_wbuf(e, p);
    }

    // hand over buffer for caller to copy into
    // leaves p locked.
    if (p->wbuf && !p->wbuf->full && p->wbuf->free >= io->len) {
        io->buf = p->wbuf->buf_pos;
        io->page_id = p->id;
        return 0;
    }

    pthread_mutex_unlock(&p->mutex);
    // p->written is incremented post-wbuf flush
    return ret;
}

/* _must_ be called after a successful write_request.
 * fills the rest of io structure.
 */
void extstore_write(void *ptr, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_page *p = &e->pages[io->page_id];

    io->offset = p->wbuf->offset + (p->wbuf->size - p->wbuf->free);
    io->page_version = p->version;
    p->wbuf->buf_pos += io->len;
    p->wbuf->free -= io->len;
    p->bytes_used += io->len;
    p->obj_count++;
    STAT_L(e);
    e->stats.bytes_written += io->len;
    e->stats.bytes_used += io->len;
    e->stats.objects_written++;
    e->stats.objects_used++;
    STAT_UL(e);

    pthread_mutex_unlock(&p->mutex);
}

/* engine submit function; takes engine, item_io stack.
 * lock io_thread context and add stack?
 * signal io thread to wake.
 * return sucess.
 */
int extstore_submit(void *ptr, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_io_thread *t = _get_io_thread(e);

    pthread_mutex_lock(&t->mutex);
    if (t->queue == NULL) {
        t->queue = io;
    } else {
        /* Have to put the *io stack at the end of current queue.
         * FIXME: Optimize by tracking tail.
         */
        obj_io *tmp = t->queue;
        while (tmp->next != NULL) {
            tmp = tmp->next;
            assert(tmp != t->queue);
        }
        tmp->next = io;
    }
    pthread_mutex_unlock(&t->mutex);

    //pthread_mutex_lock(&t->mutex);
    pthread_cond_signal(&t->cond);
    //pthread_mutex_unlock(&t->mutex);
    return 0;
}

/* engine note delete function: takes engine, page id, size?
 * note that an item in this page is no longer valid
 */
int extstore_delete(void *ptr, unsigned int page_id, uint64_t page_version,
        unsigned int count, unsigned int bytes) {
    store_engine *e = (store_engine *)ptr;
    // FIXME: validate page_id in bounds
    store_page *p = &e->pages[page_id];
    int ret = 0;

    pthread_mutex_lock(&p->mutex);
    if (!p->closed && p->version == page_version) {
        if (p->bytes_used >= bytes) {
            p->bytes_used -= bytes;
        } else {
            p->bytes_used = 0;
        }

        if (p->obj_count >= count) {
            p->obj_count -= count;
        } else {
            p->obj_count = 0; // caller has bad accounting?
        }
        STAT_L(e);
        e->stats.bytes_used -= bytes;
        e->stats.objects_used -= count;
        STAT_UL(e);

        if (p->obj_count == 0) {
            extstore_run_maint(e);
        }
    } else {
        ret = -1;
    }
    pthread_mutex_unlock(&p->mutex);
    return ret;
}

int extstore_check(void *ptr, unsigned int page_id, uint64_t page_version) {
    store_engine *e = (store_engine *)ptr;
    store_page *p = &e->pages[page_id];
    int ret = 0;

    pthread_mutex_lock(&p->mutex);
    if (p->version != page_version)
        ret = -1;
    pthread_mutex_unlock(&p->mutex);
    return ret;
}

/* allows a compactor to say "we're done with this page, kill it. */
void extstore_close_page(void *ptr, unsigned int page_id, uint64_t page_version) {
    store_engine *e = (store_engine *)ptr;
    store_page *p = &e->pages[page_id];

    pthread_mutex_lock(&p->mutex);
    if (!p->closed && p->version == page_version) {
        p->closed = true;
        extstore_run_maint(e);
    }
    pthread_mutex_unlock(&p->mutex);
}

/* Finds an attached wbuf that can satisfy the read.
 * Since wbufs can potentially be flushed to disk out of order, they are only
 * removed as the head of the list successfuly flushes to disk.
 */
// call with *p locked
// FIXME: protect from reading past wbuf
static inline int _read_from_wbuf(store_page *p, obj_io *io) {
    _store_wbuf *wbuf = p->wbuf;
    assert(wbuf != NULL);
    assert(io->offset < p->written + wbuf->size);
    if (io->iov == NULL) {
        memcpy(io->buf, wbuf->buf + (io->offset - wbuf->offset), io->len);
    } else {
        int x;
        unsigned int off = io->offset - wbuf->offset;
        // need to loop fill iovecs
        for (x = 0; x < io->iovcnt; x++) {
            struct iovec *iov = &io->iov[x];
            memcpy(iov->iov_base, wbuf->buf + off, iov->iov_len);
            off += iov->iov_len;
        }
    }
    return io->len;
}

/* engine IO thread; takes engine context
 * manage writes/reads
 * runs IO callbacks inline after each IO
 */
// FIXME: protect from reading past page
static void *extstore_io_thread(void *arg) {
    store_io_thread *me = (store_io_thread *)arg;
    store_engine *e = me->e;
    while (1) {
        obj_io *io_stack = NULL;
        pthread_mutex_lock(&me->mutex);
        if (me->queue == NULL) {
            pthread_cond_wait(&me->cond, &me->mutex);
        }

        // Pull and disconnect a batch from the queue
        if (me->queue != NULL) {
            int i;
            obj_io *end = NULL;
            io_stack = me->queue;
            end = io_stack;
            for (i = 1; i < e->io_depth; i++) {
                if (end->next) {
                    end = end->next;
                } else {
                    break;
                }
            }
            me->queue = end->next;
            end->next = NULL;
        }
        pthread_mutex_unlock(&me->mutex);

        obj_io *cur_io = io_stack;
        while (cur_io) {
            // We need to note next before the callback in case the obj_io
            // gets reused.
            obj_io *next = cur_io->next;
            int ret = 0;
            int do_op = 1;
            store_page *p = &e->pages[cur_io->page_id];
            // TODO: loop if not enough bytes were read/written.
            switch (cur_io->mode) {
                case OBJ_IO_READ:
                    // Page is currently open. deal if read is past the end.
                    pthread_mutex_lock(&p->mutex);
                    if (!p->free && !p->closed && p->version == cur_io->page_version) {
                        if (p->active && cur_io->offset >= p->written) {
                            ret = _read_from_wbuf(p, cur_io);
                            do_op = 0;
                        } else {
                            p->refcount++;
                        }
                        STAT_L(e);
                        e->stats.bytes_read += cur_io->len;
                        e->stats.objects_read++;
                        STAT_UL(e);
                    } else {
                        do_op = 0;
                        ret = -2; // TODO: enum in IO for status?
                    }
                    pthread_mutex_unlock(&p->mutex);
                    if (do_op) {
                        if (cur_io->iov == NULL) {
                            ret = pread(p->fd, cur_io->buf, cur_io->len, p->offset + cur_io->offset);
                        } else {
                            ret = preadv(p->fd, cur_io->iov, cur_io->iovcnt, p->offset + cur_io->offset);
                        }
                    }
                    break;
                case OBJ_IO_WRITE:
                    do_op = 0;
                    // FIXME: Should hold refcount during write. doesn't
                    // currently matter since page can't free while active.
                    ret = pwrite(p->fd, cur_io->buf, cur_io->len, p->offset + cur_io->offset);
                    break;
            }
            if (ret == 0) {
                E_DEBUG("read returned nothing\n");
            }

#ifdef EXTSTORE_DEBUG
            if (ret == -1) {
                perror("read/write op failed");
            }
#endif
            cur_io->cb(e, cur_io, ret);
            if (do_op) {
                pthread_mutex_lock(&p->mutex);
                p->refcount--;
                pthread_mutex_unlock(&p->mutex);
            }
            cur_io = next;
        }
    }

    return NULL;
}

// call with *p locked.
static void _free_page(store_engine *e, store_page *p) {
    store_page *tmp = NULL;
    store_page *prev = NULL;
    E_DEBUG("EXTSTORE: freeing page %u\n", p->id);
    STAT_L(e);
    e->stats.objects_used -= p->obj_count;
    e->stats.bytes_used -= p->bytes_used;
    e->stats.page_reclaims++;
    STAT_UL(e);
    pthread_mutex_lock(&e->mutex);
    // unlink page from bucket list
    tmp = e->page_buckets[p->bucket];
    while (tmp) {
        if (tmp == p) {
            if (prev) {
                prev->next = tmp->next;
            } else {
                e->page_buckets[p->bucket] = tmp->next;
            }
            tmp->next = NULL;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // reset most values
    p->version = 0;
    p->obj_count = 0;
    p->bytes_used = 0;
    p->allocated = 0;
    p->written = 0;
    p->bucket = 0;
    p->active = false;
    p->closed = false;
    p->free = true;
    // add to page stack
    p->next = e->page_freelist;
    e->page_freelist = p;
    e->page_free++;
    pthread_mutex_unlock(&e->mutex);
}

/* engine maint thread; takes engine context.
 * Uses version to ensure oldest possible objects are being evicted.
 * Needs interface to inform owner of pages with fewer objects or most space
 * free, which can then be actively compacted to avoid eviction.
 *
 * This gets called asynchronously after every page allocation. Could run less
 * often if more pages are free.
 *
 * Another allocation call is required if an attempted free didn't happen
 * due to the page having a refcount.
 */

// TODO: Don't over-evict pages if waiting on refcounts to drop
static void *extstore_maint_thread(void *arg) {
    store_maint_thread *me = (store_maint_thread *)arg;
    store_engine *e = me->e;
    struct extstore_page_data *pd =
        calloc(e->page_count, sizeof(struct extstore_page_data));
    pthread_mutex_lock(&me->mutex);
    while (1) {
        int i;
        bool do_evict = false;
        unsigned int low_page = 0;
        uint64_t low_version = ULLONG_MAX;

        pthread_cond_wait(&me->cond, &me->mutex);
        pthread_mutex_lock(&e->mutex);
        if (e->page_free == 0) {
            do_evict = true;
        }
        pthread_mutex_unlock(&e->mutex);
        memset(pd, 0, sizeof(struct extstore_page_data) * e->page_count);

        for (i = 0; i < e->page_count; i++) {
            store_page *p = &e->pages[i];
            pthread_mutex_lock(&p->mutex);
            if (p->active || p->free) {
                pthread_mutex_unlock(&p->mutex);
                continue;
            }
            if (p->obj_count > 0 && !p->closed) {
                pd[p->id].version = p->version;
                pd[p->id].bytes_used = p->bytes_used;
                if (p->version < low_version) {
                    low_version = p->version;
                    low_page = i;
                }
            }
            if ((p->obj_count == 0 || p->closed) && p->refcount == 0) {
                _free_page(e, p);
                // Found a page to free, no longer need to evict.
                do_evict = false;
            }
            pthread_mutex_unlock(&p->mutex);
        }

        if (do_evict && low_version != ULLONG_MAX) {
            store_page *p = &e->pages[low_page];
            E_DEBUG("EXTSTORE: evicting page [%d] [v: %llu]\n",
                    p->id, (unsigned long long) p->version);
            pthread_mutex_lock(&p->mutex);
            if (!p->closed) {
                p->closed = true;
                STAT_L(e);
                e->stats.page_evictions++;
                e->stats.objects_evicted += p->obj_count;
                e->stats.bytes_evicted += p->bytes_used;
                STAT_UL(e);
                if (p->refcount == 0) {
                    _free_page(e, p);
                }
            }
            pthread_mutex_unlock(&p->mutex);
        }

        // copy the page data into engine context so callers can use it from
        // the stats lock.
        STAT_L(e);
        memcpy(e->stats.page_data, pd,
                sizeof(struct extstore_page_data) * e->page_count);
        STAT_UL(e);
    }

    return NULL;
}