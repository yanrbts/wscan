/*
 * Copyright (c) 2025-2025, yanruibinghxu@gmail.com All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/mman.h>
#endif
#include <ws_malloc.h>

/* When using the libc allocator, use a minimum allocation size to match the
 * jemalloc behavior that doesn't return NULL in this case.
 */
#define MALLOC_MIN_SIZE(x) ((x) > 0 ? (x) : sizeof(long))

static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;


/* Try allocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
static inline void *ztrymalloc_usable_internal(size_t size, size_t *usable) {
    if (size >= SIZE_MAX/2) return NULL; /* Avoid overflow */

    void *ptr = malloc(MALLOC_MIN_SIZE(size));
    if (!ptr) return NULL;

    size = zmalloc_size(ptr);
    if (usable) *usable = size;
    return ptr;
}

/* Try allocating memory and zero it, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
static inline void *ztrycalloc_usable_internal(size_t size, size_t *usable) {
    if (size >= SIZE_MAX/2) return NULL; /* Avoid overflow */

    void *ptr = calloc(1, MALLOC_MIN_SIZE(size));
    if (ptr == NULL) return NULL;

    size = zmalloc_size(ptr);
    if (usable) *usable = size;
    return ptr;
}

/* Try reallocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
static inline void *ztryrealloc_usable_internal(void *ptr, size_t size, size_t *usable) {
    void *newptr;

    /* not allocating anything, just redirect to free. */
    if (size == 0 && ptr != NULL) {
        zfree(ptr);
        if (usable) *usable = 0;
        return NULL;
    }

    /* Not freeing anything, just redirect to malloc. */
    if (ptr == NULL) {
        size_t usable_size = 0;
        void *ptr = ztrymalloc_usable_internal(size, &usable_size);
        if (usable) *usable = usable_size;
        return ptr;
    }

    /* Possible overflow, return NULL, so that the caller can panic or handle a failed allocation. */
    if (size >= SIZE_MAX/2) {
        zfree(ptr);
        if (usable) *usable = 0;
        return NULL;
    }

    newptr = realloc(ptr,size);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

    size = zmalloc_size(newptr);
    if (usable) *usable = size;
    return newptr;
}

/* Allocate memory or panic */
void *zmalloc(size_t size) {
    void *ptr = ztrymalloc_usable_internal(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrymalloc(size_t size) {
    void *ptr = ztrymalloc_usable_internal(size, NULL);
    return ptr;
}

/* Allocate memory and zero it or panic */
void *zcalloc(size_t size) {
    void *ptr = ztrycalloc_usable_internal(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Allocate memory and zero it or panic.
 * We need this wrapper to have a calloc compatible signature */
void *zcalloc_num(size_t num, size_t size) {
    /* Ensure that the arguments to calloc(), when multiplied, do not wrap.
     * Division operations are susceptible to divide-by-zero errors so we also check it. */
    if ((size == 0) || (num > SIZE_MAX/size)) {
        zmalloc_oom_handler(SIZE_MAX);
        return NULL;
    }
    void *ptr = ztrycalloc_usable_internal(num*size, NULL);
    if (!ptr) zmalloc_oom_handler(num*size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrycalloc(size_t size) {
    void *ptr = ztrycalloc_usable_internal(size, NULL);
    return ptr;
}

/* Reallocate memory and zero it or panic */
void *zrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable_internal(ptr, size, NULL);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
    return ptr;
}

/* Try Reallocating memory, and return NULL if failed. */
void *ztryrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable_internal(ptr, size, NULL);
    return ptr;
}

void zfree(void *ptr) {
    if (ptr == NULL) return;
    free(ptr);
}