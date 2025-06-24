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
#ifndef __WS_EVENT_H__
#define __WS_EVENT_H__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/util.h>
#include <event2/buffer.h>

typedef void (*ws_event_cb)(int fd, short events, void *arg);
typedef void (*ws_event_http_cb)(struct evhttp_request *req, void *arg);

typedef enum {
    WS_EVENT_NORMAL = 0,
    WS_EVENT_HTTP = 1,
} ws_event_type;

typedef struct ws_event_base {
    struct event_base *base;
    int total_requests;
    int success_requests;
    int failed_requests;
} ws_event_base;

typedef struct ws_event_handle {
    ws_event_type type; // Type of the event (normal or HTTP)

    union {
        struct {
            struct event *ev;
            ws_event_cb callback;
            bool is_persistent;
        } normal;

        /* HTTP event */
        struct {
            ws_event_http_cb callback;
            /*save http request,connect and context */
            struct evhttp_connection *conn;
        } http;
    };
    
    void *arg;
} ws_event_handle;

ws_event_base *ws_event_new(void);
void ws_event_free(ws_event_base *we);
void ws_event_del(ws_event_handle *handle);
ws_event_handle *ws_event_add(ws_event_base *we, 
    int fd, short events, 
    ws_event_cb callback, void *arg, bool is_persistent);
ws_event_handle *ws_event_add_http(ws_event_base *we, 
    const char *host, int port, const char *uri, 
    ws_event_http_cb callback, void *arg);
int ws_event_loop(ws_event_base *we);
void ws_event_stop(ws_event_base *we);

#endif