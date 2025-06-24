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
#ifndef __MEMORY_UTILS_H__
#define __MEMORY_UTILS_H__

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <compiler.h>

/* The maximum value of the kernel error code is defined as 4095, 
 * that is, negative values ​​from -1 to -4095 are encoded as error pointers.*/
#define MAX_ERRNO       4095
#define IS_ERR_VALUE(x) unlikely((x) >= (unsigned long)-MAX_ERRNO)

static inline long IS_ERR_OR_NULL(const void *ptr) {
	return !ptr || IS_ERR_VALUE((unsigned long)ptr);
}

#define define_cleanup_function(type, cleaner)          \
	static inline void cleaner##_function(type *ptr)    \
	{                                                   \
		if (*ptr)                                       \
			cleaner(*ptr);                              \
	}

#define call_cleaner(cleaner)       \
	__attribute__((__cleanup__(cleaner##_function))) __attribute__((unused))

#define close_prot_errno_disarm(fd) \
	if (fd >= 0) {                  \
		int _e_ = errno;            \
		close(fd);                  \
		errno = _e_;                \
		fd = -EBADF;                \
	}

#define close_prot_errno_move(fd, new_fd)   \
	if (fd >= 0) {                          \
		int _e_ = errno;                    \
		close(fd);                          \
		errno = _e_;                        \
		fd = new_fd;                        \
		new_fd = -EBADF;	                \
	}

static inline void close_prot_errno_disarm_function(int *fd) {
       close_prot_errno_disarm(*fd);
}

#define __do_close call_cleaner(close_prot_errno_disarm)

define_cleanup_function(FILE *, fclose);
#define __do_fclose call_cleaner(fclose)

define_cleanup_function(DIR *, closedir);
#define __do_closedir call_cleaner(closedir)

#define free_disarm(ptr)                    \
	({                                      \
		if (!IS_ERR_OR_NULL(ptr)) {         \
			free(ptr);                      \
			ptr = NULL;                     \
		}                                   \
	})

static inline void free_disarm_function(void *ptr) {
	free_disarm(*(void **)ptr);
}
#define __do_free call_cleaner(free_disarm)

#define move_ptr(ptr)                           \
	({                                       	\
		typeof(ptr) __internal_ptr__ = (ptr); 	\
		(ptr) = NULL;                         	\
		__internal_ptr__;                     	\
	})

#define move_fd(fd)                         	\
	({                                  		\
		int __internal_fd__ = (fd); 			\
		(fd) = -EBADF;              			\
		__internal_fd__;            			\
	})

#endif