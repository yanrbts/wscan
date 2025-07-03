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
#ifndef __WS_REQUEST_H__
#define __WS_REQUEST_H__

#include <stddef.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_response.h>

typedef struct ws_formparam {
    char *key;
    char *value;
} ws_formparam;

typedef struct ws_fileparam {
    char *field_name;
    char *file_name;            // Original file name (for Content-Disposition filename)
    void *file_content;         // Pointer to actual file content in memory
    size_t file_content_len;    // Length of file content
} ws_fileparam;

typedef union {
    char *raw_body; // For raw POST bodies (e.g., JSON, XML)
    struct {
        ws_formparam *form_params;
        size_t num_form_params;
    };
} ws_postdata;

// Forward declaration for ws_Explorer
// typedef struct ws_explorer ws_explorer;

typedef struct ws_request {
    char *url;
    char *method;                       // "GET", "POST", "PUT", etc.
    int link_depth;                     // Depth of the link from the initial URL

    // POST/PUT related
    ws_postdata post_data;
    bool post_is_form_data;             // True if using form-data, false for raw body
    ws_fileparam *file_params;
    size_t num_file_params;

    // Headers
    struct curl_slist *extra_headers;   // Custom headers for libcurl
    char *content_type;                 // e.g., "application/json", "application/x-www-form-urlencoded"
    char *referer;

    void *user_data;                    // Custom user data to pass to completion callback
                                        // In our design, this will hold a pointer to the ws_Explorer instance.

    ws_response *response;              // Response associated with this request (will be populated on completion)
} ws_request;

/**
 * @brief Creates a new ws_Request object.
 * @param url The URL for the request.
 * @param method The HTTP method (e.g., "GET", "POST").
 * @param link_depth The depth of this link.
 * @param raw_body Raw POST/PUT body (if not form data). Set to NULL if using form data.
 * @param form_params Array of form parameters (if post_is_form_data is true). Set to NULL otherwise.
 * @param num_form_params Number of form parameters.
 * @param post_is_form_data True if post_data is form_params, false if raw_body.
 * @param file_params Array of file upload parameters. Set to NULL if no files.
 * @param num_file_params Number of file upload parameters.
 * @param content_type Content-Type header value. Can be NULL.
 * @param referer Referer header value. Can be NULL.
 * @param user_data Custom user data to pass through. Can be NULL.
 * @return A pointer to the newly created ws_Request, or NULL on failure.
 */
ws_request *ws_request_new(const char *url, const char *method, int link_depth,
                           const char *raw_body,
                           const ws_formparam *form_params, size_t num_form_params,
                           bool post_is_form_data,
                           const ws_fileparam *file_params, size_t num_file_params,
                           const char *content_type, const char *referer, void *user_data);

/**
 * @brief Adds a custom header to the request.
 * @param request The request to add the header to.
 * @param header_name The name of the header (e.g., "User-Agent").
 * @param header_value The value of the header.
 */
void ws_request_add_header(ws_request *request, const char *header_name, const char *header_value);

/**
 * @brief Frees a ws_Request object and its associated resources.
 * Also frees the associated ws_Response if it exists.
 * @param request A pointer to the ws_Request to free.
 */
void ws_request_free(ws_request *request);

// Helper for string duplication (if not already defined)
char *ws_safe_strdup(const char *s);

#endif