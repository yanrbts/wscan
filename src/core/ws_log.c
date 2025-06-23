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
#include "ws_log.h"

#define WS_MAX_CALLBACKS 32

typedef struct callback {
    ws_logfun fn;
    void *udata;
    int level;
} callback;

static struct {
    void *udata;
    ws_loglockfun lock;
    int level;
    bool quiet;
    callback callbacks[WS_MAX_CALLBACKS];
} wslog;

static const char *level_strings[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
    "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif

static void stdout_callback(ws_log *ev) {
    char buf[32];

    buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';

#ifdef LOG_USE_COLOR
    fprintf(ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
        buf, level_colors[ev->level], level_strings[ev->level],
        ev->file, ev->line);
#else
    fprintf(ev->udata, "%s %-5s %s:%d: ",
        buf, level_strings[ev->level], ev->file, ev->line);
#endif

    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");
    fflush(ev->udata);
}

static void file_callback(ws_log *ev) {
    char buf[32];

    buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';

    fprintf(ev->udata, "%s %-5s %s:%d: ",
        buf, level_strings[ev->level], ev->file, ev->line);

    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");
    fflush(ev->udata);
}

static void lock(void)   {
    if (wslog.lock) { 
        wslog.lock(true, wslog.udata);
    }
}

static void unlock(void) {
    if (wslog.lock) { 
        wslog.lock(false, wslog.udata);
    }
}

const char* log_level_string(int level) {
    return level_strings[level];
}

void ws_log_set_lock(ws_loglockfun fn, void *udata) {
    wslog.lock = fn;
    wslog.udata = udata;
}

void ws_log_set_level(int level) {
    wslog.level = level;
}

void ws_log_set_quiet(bool enable) {
    wslog.quiet = enable;
}

int ws_log_add_callback(ws_loglockfun fn, void *udata, int level) {
    for (int i = 0; i < WS_MAX_CALLBACKS; i++) {
        if (!wslog.callbacks[i].fn) {
            wslog.callbacks[i] = (callback){ fn, udata, level };
            return 0;
        }
    }

    return -1;
}

int ws_log_add_fp(FILE *fp, int level) {
    return ws_log_add_callback(file_callback, fp, level);
}

static void ws_init_event(ws_log *ev, void *udata) {
    if (!ev->time) {
        time_t t = time(NULL);
        ev->time = localtime(&t);
    }
    ev->udata = udata;
}

void ws_log_log(int level, const char *file, int line, const char *fmt, ...) {
    ws_log ev = {
        .fmt = fmt,
        .file = file,
        .line = line,
        .level = level,
    };

    lock();

    if (!wslog.quiet && level >= wslog.level) {
        ws_init_event(&ev, stderr);
        va_start(ev.ap, fmt);
        stdout_callback(&ev);
        va_end(ev.ap);
    }

    for (int i = 0; i < WS_MAX_CALLBACKS && wslog.callbacks[i].fn; i++) {
        callback *cb = &wslog.callbacks[i];
        if (level >= cb->level) {
            ws_init_event(&ev, cb->udata);
            va_start(ev.ap, fmt);
            cb->fn(&ev);
            va_end(ev.ap);
        }
    }

    unlock();
}