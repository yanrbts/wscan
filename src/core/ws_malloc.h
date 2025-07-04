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
#ifndef __WS_MALLOC_H__
#define __WS_MALLOC_H__

#include <stddef.h>

/* Includes for malloc_usable_size() */
#ifdef __FreeBSD__
#include <malloc_np.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <malloc.h>
#endif

#define zmalloc_size(p) malloc_usable_size(p)

/* 'noinline' attribute is intended to prevent the `-Wstringop-overread` warning
 * when using gcc-12 later with LTO enabled. It may be removed once the
 * bug[https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96503] is fixed. */
__attribute__((malloc,alloc_size(1),noinline)) void *zmalloc(size_t size);
__attribute__((malloc,alloc_size(1),noinline)) void *zcalloc(size_t size);
__attribute__((malloc,alloc_size(1,2),noinline)) void *zcalloc_num(size_t num, size_t size);
__attribute__((alloc_size(2),noinline)) void *zrealloc(void *ptr, size_t size);
__attribute__((malloc,alloc_size(1),noinline)) void *ztrymalloc(size_t size);
__attribute__((malloc,alloc_size(1),noinline)) void *ztrycalloc(size_t size);
__attribute__((alloc_size(2),noinline)) void *ztryrealloc(void *ptr, size_t size);
void zfree(void *ptr);

#endif