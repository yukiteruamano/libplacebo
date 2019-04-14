/* Copyright (C) 2017 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define TA_NO_WRAPPERS
#include "ta.h"

// Note: the actual minimum alignment is dictated by malloc(). It doesn't
//       make sense to set this value higher than malloc's alignment.
#define MIN_ALIGN 16

#ifndef NDEBUG
#define TA_MEMORY_DEBUGGING
#endif

struct xta_header {
    size_t size;                // size of the user allocation
    struct xta_header *prev;    // ring list containing siblings
    struct xta_header *next;
    struct xta_ext_header *ext;

#ifdef TA_MEMORY_DEBUGGING
    unsigned int canary;
    struct xta_header *leak_next;
    struct xta_header *leak_prev;
    const char *name;
#endif
};

#define CANARY 0xD3ADB3EF

union aligned_header {
    struct xta_header ta;
    // Make sure to satisfy typical alignment requirements
    void *align_ptr;
    int align_int;
    double align_d;
    long long align_ll;
    char align_min[(sizeof(struct xta_header) + MIN_ALIGN - 1) & ~(MIN_ALIGN - 1)];
};

#define PTR_TO_HEADER(ptr) (&((union aligned_header *)(ptr) - 1)->ta)
#define PTR_FROM_HEADER(h) ((void *)((union aligned_header *)(h) + 1))

#define MAX_ALLOC (((size_t)-1) - sizeof(union aligned_header))

// Needed for non-leaf allocations, or extended features such as destructors.
struct xta_ext_header {
    struct xta_header *header;  // points back to normal header
    struct xta_header children; // list of children, with this as sentinel
    void (*destructor)(void *);
};

// xta_ext_header.children.size is set to this
#define CHILDREN_SENTINEL ((size_t)-1)

static void xta_dbg_add(struct xta_header *h);
static void xta_dbg_check_header(struct xta_header *h);
static void xta_dbg_remove(struct xta_header *h);

static struct xta_header *get_header(void *ptr)
{
    struct xta_header *h = ptr ? PTR_TO_HEADER(ptr) : NULL;
    xta_dbg_check_header(h);
    return h;
}

static struct xta_ext_header *get_or_alloc_ext_header(void *ptr)
{
    struct xta_header *h = get_header(ptr);
    if (!h)
        return NULL;
    if (!h->ext) {
        h->ext = malloc(sizeof(struct xta_ext_header));
        if (!h->ext)
            return NULL;
        *h->ext = (struct xta_ext_header) {
            .header = h,
            .children = {
                .next = &h->ext->children,
                .prev = &h->ext->children,
                // Needed by xta_find_parent():
                .size = CHILDREN_SENTINEL,
                .ext = h->ext,
            },
        };
    }
    return h->ext;
}

/* Set the parent allocation of ptr. If parent==NULL, remove the parent.
 * Setting parent==NULL (with ptr!=NULL) always succeeds, and unsets the
 * parent of ptr. Operations ptr==NULL always succeed and do nothing.
 * Returns true on success, false on OOM.
 *
 * Warning: if xta_parent is a direct or indirect child of ptr, things will go
 *          wrong. The function will apparently succeed, but creates circular
 *          parent links, which are not allowed.
 */
bool xta_set_parent(void *ptr, void *xta_parent)
{
    struct xta_header *ch = get_header(ptr);
    if (!ch)
        return true;
    struct xta_ext_header *parent_eh = get_or_alloc_ext_header(xta_parent);
    if (xta_parent && !parent_eh) // do nothing on OOM
        return false;
    // Unlink from previous parent
    if (ch->next) {
        ch->next->prev = ch->prev;
        ch->prev->next = ch->next;
        ch->next = ch->prev = NULL;
    }
    // Link to new parent - insert at end of list (possibly orders destructors)
    if (parent_eh) {
        struct xta_header *children = &parent_eh->children;
        ch->next = children;
        ch->prev = children->prev;
        children->prev->next = ch;
        children->prev = ch;
    }
    return true;
}

/* Allocate size bytes of memory. If xta_parent is not NULL, this is used as
 * parent allocation (if xta_parent is freed, this allocation is automatically
 * freed as well). size==0 allocates a block of size 0 (i.e. returns non-NULL).
 * Returns NULL on OOM.
 */
void *xta_alloc_size(void *xta_parent, size_t size)
{
    if (size >= MAX_ALLOC)
        return NULL;
    struct xta_header *h = malloc(sizeof(union aligned_header) + size);
    if (!h)
        return NULL;
    *h = (struct xta_header) {.size = size};
    xta_dbg_add(h);
    void *ptr = PTR_FROM_HEADER(h);
    if (!xta_set_parent(ptr, xta_parent)) {
        xta_free(ptr);
        return NULL;
    }
    return ptr;
}

/* Exactly the same as xta_alloc_size(), but the returned memory block is
 * initialized to 0.
 */
void *xta_zalloc_size(void *xta_parent, size_t size)
{
    if (size >= MAX_ALLOC)
        return NULL;
    struct xta_header *h = calloc(1, sizeof(union aligned_header) + size);
    if (!h)
        return NULL;
    *h = (struct xta_header) {.size = size};
    xta_dbg_add(h);
    void *ptr = PTR_FROM_HEADER(h);
    if (!xta_set_parent(ptr, xta_parent)) {
        xta_free(ptr);
        return NULL;
    }
    return ptr;
}

/* Reallocate the allocation given by ptr and return a new pointer. Much like
 * realloc(), the returned pointer can be different, and on OOM, NULL is
 * returned.
 *
 * size==0 is equivalent to xta_free(ptr).
 * ptr==NULL is equivalent to xta_alloc_size(xta_parent, size).
 *
 * xta_parent is used only in the ptr==NULL case.
 *
 * Returns NULL if the operation failed.
 * NULL is also returned if size==0.
 */
void *xta_realloc_size(void *xta_parent, void *ptr, size_t size)
{
    if (size >= MAX_ALLOC)
        return NULL;
    if (!size) {
        xta_free(ptr);
        return NULL;
    }
    if (!ptr)
        return xta_alloc_size(xta_parent, size);
    struct xta_header *h = get_header(ptr);
    struct xta_header *old_h = h;
    if (h->size == size)
        return ptr;
    xta_dbg_remove(h);
    h = realloc(h, sizeof(union aligned_header) + size);
    xta_dbg_add(h ? h : old_h);
    if (!h)
        return NULL;
    h->size = size;
    if (h != old_h) {
        if (h->next) {
            // Relink siblings
            h->next->prev = h;
            h->prev->next = h;
        }
        if (h->ext) {
            // Relink children
            h->ext->header = h;
            h->ext->children.next->prev = &h->ext->children;
            h->ext->children.prev->next = &h->ext->children;
        }
    }
    return PTR_FROM_HEADER(h);
}

/* Return the allocated size of ptr. This returns the size parameter of the
 * most recent xta_alloc.../xta_realloc... call.
 * If ptr==NULL, return 0.
 */
size_t xta_get_size(void *ptr)
{
    struct xta_header *h = get_header(ptr);
    return h ? h->size : 0;
}

/* Free all allocations that (recursively) have ptr as parent allocation, but
 * do not free ptr itself.
 */
void xta_free_children(void *ptr)
{
    struct xta_header *h = get_header(ptr);
    struct xta_ext_header *eh = h ? h->ext : NULL;
    if (!eh)
        return;
    while (eh->children.next != &eh->children) {
        struct xta_header *next = eh->children.next;
        xta_free(PTR_FROM_HEADER(next));
        assert(eh->children.next != next);
    }
}

/* Free the given allocation, and all of its direct and indirect children.
 */
void xta_free(void *ptr)
{
    struct xta_header *h = get_header(ptr);
    if (!h)
        return;
    if (h->ext && h->ext->destructor)
        h->ext->destructor(ptr);
    xta_free_children(ptr);
    if (h->next) {
        // Unlink from sibling list
        h->next->prev = h->prev;
        h->prev->next = h->next;
    }
    xta_dbg_remove(h);
    free(h->ext);
    free(h);
}

/* Set a destructor that is to be called when the given allocation is freed.
 * (Whether the allocation is directly freed with xta_free() or indirectly by
 * freeing its parent does not matter.) There is only one destructor. If an
 * destructor was already set, it's overwritten.
 *
 * The destructor will be called with ptr as argument. The destructor can do
 * almost anything, but it must not attempt to free or realloc ptr. The
 * destructor is run before the allocation's children are freed (also, before
 * their destructors are run).
 *
 * Returns false if ptr==NULL, or on OOM.
 */
bool xta_set_destructor(void *ptr, void (*destructor)(void *))
{
    struct xta_ext_header *eh = get_or_alloc_ext_header(ptr);
    if (!eh)
        return false;
    eh->destructor = destructor;
    return true;
}

/* Return the ptr's parent allocation, or NULL if there isn't any.
 *
 * Warning: this has O(N) runtime complexity with N sibling allocations!
 */
void *xta_find_parent(void *ptr)
{
    struct xta_header *h = get_header(ptr);
    if (!h || !h->next)
        return NULL;
    for (struct xta_header *cur = h->next; cur != h; cur = cur->next) {
        if (cur->size == CHILDREN_SENTINEL)
            return PTR_FROM_HEADER(cur->ext->header);
    }
    return NULL;
}

#ifdef TA_MEMORY_DEBUGGING

#include <pthread.h>

static pthread_mutex_t xta_dbg_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool enable_leak_check; // pretty much constant
static struct xta_header leak_node;
static char allocation_is_string;

static void xta_dbg_add(struct xta_header *h)
{
    h->canary = CANARY;
    if (enable_leak_check) {
        pthread_mutex_lock(&xta_dbg_mutex);
        h->leak_next = &leak_node;
        h->leak_prev = leak_node.leak_prev;
        leak_node.leak_prev->leak_next = h;
        leak_node.leak_prev = h;
        pthread_mutex_unlock(&xta_dbg_mutex);
    }
}

static void xta_dbg_check_header(struct xta_header *h)
{
    if (h)
        assert(h->canary == CANARY);
}

static void xta_dbg_remove(struct xta_header *h)
{
    xta_dbg_check_header(h);
    if (h->leak_next) { // assume checking for !=NULL invariant ok without lock
        pthread_mutex_lock(&xta_dbg_mutex);
        h->leak_next->leak_prev = h->leak_prev;
        h->leak_prev->leak_next = h->leak_next;
        pthread_mutex_unlock(&xta_dbg_mutex);
        h->leak_next = h->leak_prev = NULL;
    }
    h->canary = 0;
}

static size_t get_children_size(struct xta_header *h)
{
    size_t size = 0;
    if (h->ext) {
        struct xta_header *s;
        for (s = h->ext->children.next; s != &h->ext->children; s = s->next)
            size += s->size + get_children_size(s);
    }
    return size;
}

void xta_print_leak_report(void)
{
    if (!enable_leak_check)
        return;

    pthread_mutex_lock(&xta_dbg_mutex);
    if (leak_node.leak_next && leak_node.leak_next != &leak_node) {
        size_t size = 0;
        size_t num_blocks = 0;
        fprintf(stderr, "Blocks not freed:\n");
        fprintf(stderr, "  %-20s %10s %10s  %s\n",
                "Ptr", "Bytes", "C. Bytes", "Name");
        while (leak_node.leak_next != &leak_node) {
            struct xta_header *cur = leak_node.leak_next;
            // Don't list those with parent; logically, only parents are listed
            if (!cur->next) {
                size_t c_size = get_children_size(cur);
                char name[256] = {0};
                if (cur->name)
                    snprintf(name, sizeof(name), "%s", cur->name);
                if (cur->name == &allocation_is_string) {
                    snprintf(name, sizeof(name), "'%.*s'",
                             (int)cur->size, (char *)PTR_FROM_HEADER(cur));
                }
                for (int n = 0; n < sizeof(name); n++) {
                    if (name[n] && name[n] < 0x20)
                        name[n] = '.';
                }
                fprintf(stderr, "  %-20p %10zu %10zu  %s\n",
                        cur, cur->size, c_size, name);
            }
            size += cur->size;
            num_blocks += 1;
            // Unlink, and don't confuse valgrind by leaving live pointers.
            assert(cur->leak_next && cur->leak_prev);
            cur->leak_next->leak_prev = cur->leak_prev;
            cur->leak_prev->leak_next = cur->leak_next;
            cur->leak_next = cur->leak_prev = NULL;
        }
        fprintf(stderr, "%zu bytes in %zu blocks.\n", size, num_blocks);
    }
    pthread_mutex_unlock(&xta_dbg_mutex);
}

void xta_enable_leak_report(void)
{
    pthread_mutex_lock(&xta_dbg_mutex);
    enable_leak_check = true;
    if (!leak_node.leak_prev && !leak_node.leak_next) {
        leak_node.leak_prev = &leak_node;
        leak_node.leak_next = &leak_node;
    }
    pthread_mutex_unlock(&xta_dbg_mutex);
}

/* Set a (static) string that will be printed if the memory allocation in ptr
 * shows up on the leak report. The string must stay valid until ptr is freed.
 * Calling it on ptr==NULL does nothing.
 * Typically used to set location info.
 * Always returns ptr (useful for chaining function calls).
 */
void *xta_dbg_set_loc(void *ptr, const char *loc)
{
    struct xta_header *h = get_header(ptr);
    if (h)
        h->name = loc;
    return ptr;
}

/* Mark the allocation as string. The leak report will print it literally.
 */
void *xta_dbg_mark_as_string(void *ptr)
{
    // Specially handled by leak report code.
    return xta_dbg_set_loc(ptr, &allocation_is_string);
}

#else

static void xta_dbg_add(struct xta_header *h){}
static void xta_dbg_check_header(struct xta_header *h){}
static void xta_dbg_remove(struct xta_header *h){}

void xta_print_leak_report(void){}
void xta_enable_leak_report(void){}
void *xta_dbg_set_loc(void *ptr, const char *loc){return ptr;}
void *xta_dbg_mark_as_string(void *ptr){return ptr;}

#endif
