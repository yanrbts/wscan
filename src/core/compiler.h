/* Copyright (c) 2024-2024, yanruibinghxu@gmail.com All rights reserved.
 * SPDX-License-Identifier: LGPL-2.1+ 
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
#ifndef __COMPILER_H__
#define __COMPILER_H__

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/types.h>
#include <stdbool.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

/* Used to tell the compiler that a condition is more likely to be true */
#define likely(x) __builtin_expect(!!(x), 1)

/* Indicates a situation that is "unlikely to happen" */
#define unlikely(x) __builtin_expect(!!(x), 0)

/* It is mandatory to check the return value of a function. 
 * If the return value is not used when calling this function, 
 * the compiler will issue a warning */
#define __must_check __attribute__((__warn_unused_result__))

/*
 * Avoids triggering -Wtype-limits compilation warning,
 * while using unsigned data types to check a < 0.
 */
#define is_non_negative(a) ((a) > 0 || (a) == 0)
#define is_negative(a) (!(is_non_negative(a)))

#ifndef __hidden
#define __hidden __attribute__((visibility("hidden")))
#endif

#ifndef __public
#define __public __attribute__((visibility("default")))
#endif

/* This attribute is required to silence clang warnings 
 * This variable or function may not be used, 
 * please do not report "unused" warnings */
#if defined(__GNUC__)
#define _unused __attribute__((unused))
#else
#define _unused
#endif

#ifndef __noreturn
#	if __STDC_VERSION__ >= 201112L
#		define __noreturn _Noreturn
#	else
#		define __noreturn __attribute__((noreturn))
#	endif
#endif

/* Used to tell the compiler "this function is likely 
 * to be called frequently". This allows the compiler 
 * to optimize */
#ifndef __hot
#define __hot __attribute__((hot))
#endif

#endif