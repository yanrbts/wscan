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
 * SUBSTITUTE GOODS OR OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __WS_HTTP_H__
#define __WS_HTTP_H__

#include <stdbool.h>
#include <curl/curl.h>
#include <ws_event.h>

// Forward declarations for opaque types
typedef struct ws_http_client   ws_http_client_t;
typedef struct ws_http_request  ws_http_request_t;

/**
 * @brief Callback for when HTTP response headers are received.
 * This callback is optional. If provided, it will be called for each header line.
 *
 * @param header The header line (e.g., "Content-Type: text/html").
 * @param user_data Custom data passed by the user during request creation.
 */
typedef void (*ws_http_header_cb_fn)(const char *header, void *user_data);

/**
 * @brief Callback for when HTTP response body data is received.
 * This callback can be called multiple times for a single response.
 *
 * @param data Pointer to the received data.
 * @param size Size of the received data in bytes.
 * @param user_data Custom data passed by the user during request creation.
 */
typedef void (*ws_http_data_cb_fn)(const char *data, size_t size, void *user_data);

/**
 * @brief Callback for when an HTTP request completes (success or failure).
 *
 * @param request The completed HTTP request object.
 * @param http_code The HTTP status code (e.g., 200, 404). 0 if no HTTP response was received.
 * @param curl_code The CURLcode indicating libcurl's result (e.g., CURLE_OK).
 * @param user_data Custom data passed by the user during request creation.
 */
typedef void (*ws_http_complete_cb_fn)(ws_http_request_t *request, long http_code, CURLcode curl_code, void *user_data);

/**
 * @brief Creates a new HTTP client instance.
 *
 * This client manages multiple concurrent HTTP requests using libcurl's multi interface.
 * It requires an initialized ws_event_loop_t to integrate with.
 *
 * @param loop The event loop to use for non-blocking I/O.
 * @return A pointer to the new HTTP client, or NULL on failure.
 */
ws_http_client_t *ws_http_client_new(ws_event_loop_t *loop);

/**
 * @brief Frees an HTTP client instance and all its associated resources.
 * Any pending requests will be cancelled and their callbacks will NOT be invoked.
 *
 * @param client The HTTP client to free.
 */
void ws_http_client_free(ws_http_client_t *client);

/**
 * @brief Performs an asynchronous HTTP GET request.
 *
 * @param client The HTTP client instance.
 * @param url The URL to request.
 * @param header_cb Optional callback for response headers. Can be NULL.
 * @param data_cb Optional callback for response body data. Can be NULL.
 * @param complete_cb Required callback for request completion (success/failure). Cannot be NULL.
 * @param user_data Custom data to pass to all callbacks for this request.
 * @return A pointer to the HTTP request object, or NULL on failure.
 * The request object is managed internally and freed upon completion.
 */
ws_http_request_t *ws_http_get(ws_http_client_t *client, const char *url,
                               ws_http_header_cb_fn header_cb,
                               ws_http_data_cb_fn data_cb,
                               ws_http_complete_cb_fn complete_cb,
                               void *user_data);

/**
 * @brief Performs an asynchronous HTTP POST request.
 *
 * @param client The HTTP client instance.
 * @param url The URL to post to.
 * @param post_data The data to send in the POST body.
 * @param post_data_len The length of the POST data.
 * @param header_cb Optional callback for response headers. Can be NULL.
 * @param data_cb Optional callback for response body data. Can be NULL.
 * @param complete_cb Required callback for request completion (success/failure). Cannot be NULL.
 * @param user_data Custom data to pass to all callbacks for this request.
 * @return A pointer to the HTTP request object, or NULL on failure.
 * The request object is managed internally and freed upon completion.
 */
ws_http_request_t *ws_http_post(ws_http_client_t *client, const char *url,
                                const char *post_data, size_t post_data_len,
                                ws_http_header_cb_fn header_cb,
                                ws_http_data_cb_fn data_cb,
                                ws_http_complete_cb_fn complete_cb,
                                void *user_data);

/**
 * @brief Cancels a specific HTTP request if it is still in progress.
 * The complete callback for this request will NOT be invoked.
 * The request object will be freed.
 *
 * @param client The HTTP client instance.
 * @param request The HTTP request to cancel.
 * @return true if the request was found and cancelled, false otherwise.
 */
bool ws_http_cancel_request(ws_http_client_t *client, ws_http_request_t *request);

#endif // __WS_HTTP_H__