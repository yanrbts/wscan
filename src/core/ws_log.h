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
#ifndef __WS_LOG_H__
#define __WS_LOG_H__

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

typedef struct ws_log {
    va_list ap;
    const char *fmt;
    const char *file;
    struct tm *time;
    void *udata;
    int line;
    int level;
} ws_log;

typedef void (*ws_logfun)(ws_log *ev);
typedef void (*ws_loglockfun)(bool lock, void *udata);

enum { 
    LOG_TRACE=0, 
    LOG_DEBUG=1, 
    LOG_INFO=2, 
    LOG_WARN=3, 
    LOG_ERROR=4, 
    LOG_FATAL=5 
};

#define ws_log_trace(...) ws_log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define ws_log_debug(...) ws_log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define ws_log_info(...)  ws_log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define ws_log_warn(...)  ws_log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define ws_log_error(...) ws_log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define ws_log_fatal(...) ws_log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

const char* ws_log_level_string(int level);
void ws_log_set_lock(ws_loglockfun fn, void *udata);
void ws_log_set_level(int level);
void ws_log_set_quiet(bool enable);
int ws_log_add_callback(ws_logfun fn, void *udata, int level);
int ws_log_add_fp(FILE *fp, int level);
void ws_log_log(int level, const char *file, int line, const char *fmt, ...);

#endif