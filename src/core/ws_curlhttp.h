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
#ifndef __WS_CURLHTTP_H__
#define __WS_CURLHTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <curl/curl.h>
#include <ws_event.h>
#include <ws_request.h>

// Internal context for each active CURL easy handle
typedef struct ws_http_request_context {
    CURL *easy_handle;
    CURLM *multi_handle;                        // Pointer to the shared multi handle
    struct ws_http_manager_ctx *manager_ctx;    // Pointer to the manager context
    ws_request *original_request;               // Original request data
    ws_event_http_cb completion_callback;       // Callback to explorer
    ws_event_base *event_base;                  // Pointer to the event base for scheduling events
    // libevent events for this socket (pointers to ws_event_handle, owned by ws_event_base)
    ws_event_handle *socket_event_read;
    ws_event_handle *socket_event_write;
    // Custom headers list, must be freed by us if not set on easy_handle (or by curl if set)
    struct curl_slist *current_headers_list;
} ws_http_request_context;

/**
 * @brief Structure to pass to CURLMOPT_TIMERDATA and CURLMOPT_SOCKETDATA callbacks.
 */
typedef struct ws_http_manager_ctx {
    CURLM *multi_handle;
    ws_event_base *event_base;
    ws_event_handle *curl_multi_timer_event_handle; // The ws_event_handle for the multi timer
} ws_http_manager_ctx;

/**
 * @brief Initializes the HTTP client multi-handle and sets up callbacks.
 * @param we The ws_event_base to integrate with.
 * @return A ws_http_manager_ctx* handle on success, NULL on failure.
 * This context holds the CURLM* and other necessary data.
 */
ws_http_manager_ctx *ws_http_init(ws_event_base *we);

/**
 * @brief Cleans up the HTTP client multi-handle and manager context.
 * @param manager_ctx The ws_http_manager_ctx* handle to cleanup.
 */
void ws_http_cleanup(ws_http_manager_ctx *manager_ctx);

/**
 * @brief Performs an HTTP request asynchronously using libcurl and ws_event.
 * @param manager_ctx The ws_http_manager_ctx* containing the CURLM* handle.
 * @param request The ws_Request object containing request details.
 * @param callback The completion callback function.
 * @return True on success, false on failure to add the request.
 */
bool ws_http_perform_request(ws_http_manager_ctx *manager_ctx,
                             ws_request *request, ws_event_http_cb callback);


#endif