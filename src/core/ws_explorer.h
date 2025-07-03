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
#ifndef __WS_EXPLORER_H__
#define __WS_EXPLORER_H__

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <curl/curl.h>

#include <ws_event.h>
#include <ws_request.h>
#include <ws_rbtree.h>
#include <ws_curlhttp.h>

// RequestNode (Linked list for queue)
typedef struct ws_requestnode {
    ws_request *request;
    struct ws_requestnode *next;
} ws_requestnode;

typedef struct ws_explorer {
    ws_http_manager_ctx *http_manager; // libcurl multi interface handle and manager context
    ws_event_base *event_base;  // Your custom event base
    ws_requestnode *queue_head; // Request queue (simple linked list)
    ws_requestnode *queue_tail;
    rbTable *visited_urls;      // Set of already processed URLs (using red-black tree with string hash/key)
    int max_depth;              // Mimic Python Explorer properties
    size_t max_page_size;       // bytes (currently not enforced in ws_http.c)
    int active_easy_handles;    // Number of currently active (running) easy handles
    int parallelism;            // Limit the number of concurrent requests
    bool stop_flag;             // A flag to indicate if the explorer should stop
} ws_explorer;

// Function declarations
ws_explorer* ws_explorer_new(int max_depth, size_t max_page_size, int parallelism);
void ws_explorer_add_to_queue(ws_explorer *explorer, ws_request *request);
bool ws_explorer_has_visited(ws_explorer *explorer, const char *url);
void ws_explorer_mark_visited(ws_explorer *explorer, const char *url);

// Callback from ws_http.c when a request completes
static void ws_explorer_http_completion_cb(int status_code, struct evkeyvalq *headers,
                                            const char *body, size_t body_len,
                                            void *user_data, int error_code);

// Core exploration loop
void ws_explorer_explore(ws_explorer *explorer, ws_request *initial_request);
void ws_explorer_stop(ws_explorer *explorer);

void ws_explorer_free(ws_explorer *explorer);

// Link extraction placeholder
ws_request** ws_extract_links(const ws_response *response, const ws_request *original_request, size_t *num_links_out);

#endif