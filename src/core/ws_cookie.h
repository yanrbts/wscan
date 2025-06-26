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

#ifndef __WS_COOKIE_H__
#define __WS_COOKIE_H__

#include <stdint.h>
#include <stdbool.h>
#include <time.h> // For time_t
#include <sys/queue.h>
#include <event2/keyvalq_struct.h> // For struct evkeyvalq (for Set-Cookie headers)
#include <ws_rbtree.h>

typedef struct ws_cookie {
    char *name;                     // Cookie name
    char *value;                    // Cookie value
    char *domain;                   // Domain (e.g., "example.com" or ".example.com")
    char *path;                     // Path (e.g., "/", "/docs")
    time_t expires;                 // Expiration timestamp (seconds since epoch), 0 if session cookie
    bool secure;                    // Secure flag (only send over HTTPS)
    bool httponly;                  // HttpOnly flag (not accessible by client-side scripts)
    TAILQ_ENTRY(ws_cookie) next;    // For linked list within a path
} ws_cookie;

// A list of ws_cookie for a specific domain and path
TAILQ_HEAD(ws_cookie_list_head, ws_cookie);

// --- ws_path_map_item: Item stored in the 'path_cookies' RBTree ---
// This struct will serve as the actual item (value) stored in the RBTree.
// It contains the path string (key) and a pointer to the TAILQ of cookies.
typedef struct ws_path_map_item {
    char *path_key; // The path string, serves as the key for comparison in path_cookies RBTree
    struct ws_cookie_list_head *cookie_list_head; // The TAILQ of cookies associated with this path
} ws_path_map_item;

/* 
 * --- Per-Domain Cookie Storage ---
 * RBTree: Key = char* (path string), Value = ws_cookie_list_head*
 */
typedef struct ws_domain_cookies {
    char *domain;
    rbTable *path_cookies; // Tree of paths, each pointing to a list of cookies
} ws_domain_cookies;

/* 
 * --- Main Cookie Jar Structure ---
 * RBTree: Key = char* (domain string), Value = ws_domain_cookies* 
 */
typedef struct ws_cookie_jar {
    rbTable *domain_map; // Top-level tree mapping domains to their cookie storage
} ws_cookie_jar;

/**
 * @brief Creates a new Cookie Jar.
 * @return A pointer to the newly created ws_cookie_jar struct, or NULL on failure.
 */
ws_cookie_jar *ws_cookie_jar_new(void);

/**
 * @brief Frees the Cookie Jar and all stored cookies.
 * @param jar The Cookie Jar to free.
 */
void ws_cookie_jar_free(ws_cookie_jar *jar);

/**
 * @brief Parses "Set-Cookie" headers from an HTTP response and adds them to the Cookie Jar.
 * This function handles parsing of various cookie attributes (Domain, Path, Expires, etc.).
 * @param jar The Cookie Jar instance.
 * @param request_host The host name of the original request (used if Set-Cookie doesn't specify Domain).
 * @param request_path The path of the original request (used if Set-Cookie doesn't specify Path).
 * @param is_https True if the request was made over HTTPS, false otherwise.
 * @param set_cookie_headers The list of "Set-Cookie" headers from the response.
 */
void ws_cookie_jar_parse_set_cookie_headers(ws_cookie_jar *jar,
                                            const char *request_host,
                                            const char *request_path,
                                            bool is_https,
                                            struct evkeyvalq *set_cookie_headers);

/**
 * @brief Retrieves applicable cookies for a given host and path, and formats them into a "Cookie" header string.
 * The returned string is dynamically allocated and must be freed by the caller using `zfree()`.
 * @param jar The Cookie Jar instance.
 * @param request_host The host name of the request.
 * @param request_path The path of the request.
 * @param is_https True if the request is being made over HTTPS, false otherwise.
 * @return A dynamically allocated string containing the "name=value; name2=value2" formatted cookies, or NULL if no applicable cookies are found.
 */
char *ws_cookie_jar_get_cookie_header_string(ws_cookie_jar *jar,
                                             const char *request_host,
                                             const char *request_path,
                                             bool is_https);

#endif