/*
 * Copyright (c) 2025-2025, yanruibinghxu@gmail.com All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * * Neither the name of Redis nor the names of its contributors may be used
 * to endorse or promote products derived from this software without
 * specific prior written permission.
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

#ifndef __WS_HTTP_H__
#define __WS_HTTP_H__

#include <event2/keyvalq_struct.h> // For struct evkeyvalq
#include <ws_event.h>

// Define HTTP request method enum
typedef enum {
    WS_HTTP_GET,
    WS_HTTP_POST,
    WS_HTTP_PUT,
    WS_HTTP_DELETE,
} ws_http_method;

// HTTP Request parameters structure (replaces original host, port, uri parameters)
typedef struct ws_http_request_options {
    ws_http_method method;               // Request method (GET, POST, etc.)
    const char *url;                     // Full URL (e.g., "http://example.com/path?query=val")
    struct evkeyvalq *headers;           // Optional custom request headers
    const char *body_data;               // POST/PUT request body data (optional)
    size_t body_len;                     // Length of POST/PUT request body data
    long timeout_ms;                     // Request timeout in milliseconds (0 means default or no timeout)
    bool follow_redirects;               // Whether to automatically follow redirects (requires additional logic)
} ws_http_request_options;

/**
 * @brief Sends an HTTP request to the event loop.
 * This function creates a new HTTP request and triggers a callback when the
 * request is complete. It allows for asynchronous handling of HTTP requests,
 * useful for web applications or services.
 * @param we A pointer to the ws_event_base structure.
 * @param options A pointer to a structure containing the request configuration.
 * @param callback The callback function to call when the HTTP request completes.
 * @param arg An argument to pass to the callback function.
 * @return A pointer to the newly created ws_event_handle structure if the request
 * is successfully initiated, or NULL on failure. The request result will
 * be returned asynchronously in the callback function.
 */
ws_event_handle *ws_event_http_request(ws_event_base *we,
                                    const ws_http_request_options *options,
                                    ws_event_http_cb callback, void *arg);

#endif