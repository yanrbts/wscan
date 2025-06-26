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
#include <string.h>
#include <stdio.h>
#include <stdlib.h> // For strtoll
#include <sys/queue.h>
#include <ctype.h> // For isspace, tolower
#include <time.h> // For time()
#include <event2/keyvalq_struct.h> // For struct evkeyvalq
#include <event2/event.h>
#include <event2/http.h>
#include <event2/util.h>
#include <event2/buffer.h>
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_util.h>
#include <ws_cookie.h>

// Month mapping table
static int parse_month(const char *mon) {
    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    for (int i = 0; i < 12; ++i) {
        if (strcasecmp(mon, months[i]) == 0) return i;
    }
    return -1;
}

// Parse 2-digit year like '94' to 1994 or 2094
static int parse_year_2digit(int y) {
    return (y < 70) ? (2000 + y) : (1900 + y);
}

// Core parsing function
static time_t ws_parse_http_date(const char *date_str) {
    if (!date_str || !*date_str) return 0;

    struct tm tm = {0};
    char wkday[10], month[4], tz[4];
    char tmp[40]; // scratch buffer
    int day, year, hour, min, sec;

    // --- Try RFC 1123: "Sun, 06 Nov 1994 08:49:37 GMT" ---
    if (sscanf(date_str, "%9[^,], %d %3s %d %d:%d:%d %3s",
               wkday, &day, month, &year, &hour, &min, &sec, tz) == 8) {
        int mon = parse_month(month);
        if (mon < 0) return 0;
        tm.tm_mday = day;
        tm.tm_mon = mon;
        tm.tm_year = year - 1900;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        return timegm(&tm); // UTC-safe
    }

    // --- Try RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT" ---
    if (sscanf(date_str, "%9[^,], %d-%3s-%d %d:%d:%d %3s",
               wkday, &day, month, &year, &hour, &min, &sec, tz) == 8) {
        int mon = parse_month(month);
        if (mon < 0) return 0;
        tm.tm_mday = day;
        tm.tm_mon = mon;
        tm.tm_year = parse_year_2digit(year) - 1900;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        return timegm(&tm);
    }

    // --- Try ANSI C asctime(): "Sun Nov  6 08:49:37 1994" ---
    if (sscanf(date_str, "%3s %3s %d %d:%d:%d %d",
               wkday, month, &day, &hour, &min, &sec, &year) == 7) {
        int mon = parse_month(month);
        if (mon < 0) return 0;
        tm.tm_mday = day;
        tm.tm_mon = mon;
        tm.tm_year = year - 1900;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        return timegm(&tm);
    }

    return 0; // Unrecognized format
}
static void ws_cookie_free(ws_cookie *cookie) {
    if (cookie) {
        if (cookie->name) zfree(cookie->name);
        if (cookie->domain) zfree(cookie->domain);
        if (cookie->value) zfree(cookie->value);
        if (cookie->path) zfree(cookie->path);
        zfree(cookie);
    }
}

static void ws_cookie_path_item_free(void *item, void *param) {
    UNUSED(param);

    ws_path_map_item *path_item = (ws_path_map_item *)item;
    if (path_item) {
        /* Free the path string (key) */
        zfree(path_item->path_key);

        /* Iterate and free all cookies in the TAILQ. */
        if (path_item->cookie_list_head) {
            ws_cookie *c;

            while (!TAILQ_EMPTY(path_item->cookie_list_head)) {
                c = TAILQ_FIRST(path_item->cookie_list_head);
                TAILQ_REMOVE(path_item->cookie_list_head, c, next);
                ws_cookie_free(c);
            }
            zfree(path_item->cookie_list_head);
        }
        zfree(path_item);
    }
}

// Free function for ws_domain_cookies (which acts as the item for domain_map).
// This is called by rbDestroy on the 'domain_map' RBTree.
// It frees the domain string, recursively destroys the nested 'path_cookies' RBTree,
// and then frees the ws_domain_cookies struct itself.
static void ws_cookie_domain_item_free(void *item, void *param) {
    UNUSED(param);

    ws_domain_cookies *domain_item = (ws_domain_cookies *)item;
    if (domain_item) {
        zfree(domain_item->domain);

        if (domain_item->path_cookies) {
            rbDestroy(domain_item->path_cookies, ws_cookie_path_item_free);
        }
        zfree(domain_item);
    }
}

// RBTree comparison for domain keys (case-insensitive).
// Compares two ws_domain_cookies items based on their 'domain' field.
static int ws_cookie_domain_item_cmp(const void *a, const void *b, void *param) {
    UNUSED(param);
    const ws_domain_cookies *item_a = (const ws_domain_cookies *)a;
    const ws_domain_cookies *item_b = (const ws_domain_cookies *)b;
    // Use ws_strcasecmp for case-insensitive domain comparison
    return ws_strcasecmp(item_a->domain, item_b->domain);
}

// RBTree comparison for path keys (case-sensitive as paths are usually case-sensitive).
// Compares two ws_path_map_item items based on their 'path_key' field.
static int ws_cookie_path_item_cmp(const void *a, const void *b, void *param) {
    UNUSED(param);
    const ws_path_map_item *item_a = (const ws_path_map_item *)a;
    const ws_path_map_item *item_b = (const ws_path_map_item *)b;
    // Use strcmp for case-sensitive path comparison
    return strcmp(item_a->path_key, item_b->path_key);
}

/**
 * @brief Creates a new Cookie Jar.
 */
ws_cookie_jar *ws_cookie_jar_new(void) {
    ws_cookie_jar *jar = zmalloc(sizeof(ws_cookie_jar));
    if (!jar) {
        ws_log_error("Failed to allocate memory for ws_cookie_jar.");
        return NULL;
    }

    // Create the domain_map RBTree, providing the comparison and item free functions.
    // rbCreate takes comparison function, its param.
    jar->domain_map = rbCreate(ws_cookie_domain_item_cmp, NULL);
    if (!jar->domain_map) {
        ws_log_error("Failed to create domain_map for cookie jar.");
        zfree(jar);
        return NULL;
    }

    return jar;
}

/**
 * @brief Frees the Cookie Jar and all stored cookies.
 */
void ws_cookie_jar_free(ws_cookie_jar *jar) {
    if (jar) {
        if (jar->domain_map) {
            rbDestroy(jar->domain_map, ws_cookie_domain_item_free); // Will use defined free funcs
        }
        zfree(jar);
    }
}

/**
 * Parses a single "Set-Cookie" header string and returns a dynamically allocated ws_cookie struct.
 * On failure (e.g. bad format, memory allocation error), returns NULL.
 *
 * @param cookie_str       The raw Set-Cookie header string, e.g. "SID=abc123; Path=/; HttpOnly".
 * @param default_domain   The default domain to assign if no Domain attribute is specified.
 * @param default_path     The default path to assign if no Path attribute is specified.
 *
 * @return A pointer to a heap-allocated ws_cookie struct, or NULL on error.
 */
static ws_cookie *parse_set_cookie_string(const char *cookie_str, 
    const char *default_domain, const char *default_path)
{
    if (!cookie_str || !default_domain || !default_path) {
        return NULL; // Invalid input
    }

    char *cookie_copy = NULL;
    // Allocate and initialize a new ws_cookie struct
    ws_cookie *cookie = zcalloc(sizeof(ws_cookie));
    if (!cookie) {
        ws_log_error("Failed to allocate memory for new cookie.");
        return NULL;
    }

    // Initialize default fields
    cookie->expires = 0;         // 0 means session cookie
    cookie->secure = false;      // By default not Secure
    cookie->httponly = false;    // By default not HttpOnly

    // Set default domain and path (can be overridden later)
    if (!(cookie->domain = strdup(default_domain)) || !(cookie->path = strdup(default_path))) {
        ws_log_error("Failed to duplicate default domain/path for cookie.");
        goto err;
    }

    // Create a mutable copy of the cookie string for parsing
    cookie_copy = strdup(cookie_str);
    if (!cookie_copy) {
        ws_log_error("Failed to duplicate Set-Cookie string.");
        goto err;
    }

    // --- Parse the first token: Name=Value pair ---
    char *token = cookie_copy;
    char *next_token = strchr(token, ';'); // Look for first semicolon to split
    if (next_token) {
        *next_token++ = '\0'; // Null-terminate the first part
    }

    char *eq = strchr(token, '=');
    if (!eq) {
        // The first part must be name=value
        ws_log_warn("Invalid Set-Cookie: Missing '=' in name-value pair: %s", token);
        goto err;
    }

    *eq = '\0'; // Split into name and value
    cookie->name = strdup(ws_trim_whitespace(token));       // Trim and copy name
    cookie->value = strdup(ws_trim_whitespace(eq + 1));     // Trim and copy value
    if (!cookie->name || !cookie->value) {
        ws_log_error("Failed to duplicate cookie name/value.");
        goto err;
    }

    // --- Parse the remaining attributes ---
    token = next_token;
    while (token && *token) {
        token = ws_trim_whitespace(token);        // Remove leading spaces
        next_token = strchr(token, ';');          // Look for next attribute
        if (next_token) {
            *next_token++ = '\0';                 // Null-terminate this attribute
        }

        // Split attribute into name=value or just flag
        char *attr_eq = strchr(token, '=');
        char *attr_name = token;
        char *attr_value = NULL;

        if (attr_eq) {
            *attr_eq = '\0';                      // Split attribute name and value
            attr_value = ws_trim_whitespace(attr_eq + 1);
        }
        attr_name = ws_trim_whitespace(attr_name);

        // Handle standard cookie attributes (case-insensitive per RFC)
        if (ws_strcasecmp(attr_name, "Domain") == 0 && attr_value) {
            // Domain attribute: override default domain
            zfree(cookie->domain);
            // Per RFC 6265 §4.1.2.3: domain starting with '.' is allowed, but must be stored without it
            cookie->domain = strdup(attr_value[0] == '.' ? attr_value + 1 : attr_value);
        } else if (ws_strcasecmp(attr_name, "Path") == 0 && attr_value) {
            // Path attribute: override default path
            zfree(cookie->path);
            cookie->path = strdup(attr_value);
        } else if (ws_strcasecmp(attr_name, "Expires") == 0 && attr_value) {
            // Expires attribute: parse HTTP date string into epoch seconds
            cookie->expires = ws_parse_http_date(attr_value);
        } else if (ws_strcasecmp(attr_name, "Max-Age") == 0 && attr_value) {
            // Max-Age: relative expiry in seconds from now
            long long max_age = strtoll(attr_value, NULL, 10);
            cookie->expires = (max_age >= 0) ? time(NULL) + max_age : 1; // If <=0, set to "past"
        } else if (ws_strcasecmp(attr_name, "Secure") == 0) {
            // Secure flag: cookie should only be sent over HTTPS
            cookie->secure = true;
        } else if (ws_strcasecmp(attr_name, "HttpOnly") == 0) {
            // HttpOnly flag: cookie not accessible to JavaScript
            cookie->httponly = true;
        }
        // Unknown attributes are ignored (e.g. SameSite)

        token = next_token;
    }

    // Free the temporary mutable copy
    zfree(cookie_copy);
    return cookie;

err:
    if (cookie_copy) zfree(cookie_copy);
    if (cookie) ws_cookie_free(cookie);
    return NULL;
}

static bool validate_cookie_domain(ws_cookie *cookie, const char *request_host) {
    if (!cookie->domain) {
        cookie->domain = strdup(request_host);
        return cookie->domain != NULL;
    }

    size_t hlen = strlen(request_host);
    size_t dlen = strlen(cookie->domain);
    if (hlen < dlen)
        return false;

    if (ws_strcasecmp(request_host, cookie->domain) == 0)
        return true;

    if (hlen > dlen &&
        request_host[hlen - dlen - 1] == '.' &&
        ws_strcasecmp(request_host + hlen - dlen, cookie->domain) == 0)
        return true;

    ws_log_warn("Invalid cookie domain '%s' for host '%s'", cookie->domain, request_host);
    return false;
}

static ws_domain_cookies *get_or_create_domain_entry(ws_cookie_jar *jar, const char *domain) {
    ws_domain_cookies key = { .domain = (char *)domain };
    ws_domain_cookies *entry = (ws_domain_cookies *)rbFind(jar->domain_map, &key);
    if (entry) return entry;

    entry = zmalloc(sizeof(ws_domain_cookies));
    if (!entry) return NULL;

    entry->domain = strdup(domain);
    if (!entry->domain) {
        zfree(entry);
        return NULL;
    }

    entry->path_cookies = rbCreate(ws_cookie_path_item_cmp, NULL);
    if (!entry->path_cookies) {
        zfree(entry->domain);
        zfree(entry);
        return NULL;
    }

    if (!rbProbe(jar->domain_map, entry)) {
        rbDestroy(entry->path_cookies, NULL);
        zfree(entry->domain);
        zfree(entry);
        return NULL;
    }

    return entry;
}

static ws_path_map_item *get_or_create_path_entry(ws_domain_cookies *domain_entry, const char *path) {
    ws_path_map_item key = { .path_key = (char *)path };
    ws_path_map_item *entry = (ws_path_map_item *)rbFind(domain_entry->path_cookies, &key);
    if (entry) return entry;

    entry = zmalloc(sizeof(ws_path_map_item));
    if (!entry) return NULL;

    entry->path_key = strdup(path);
    if (!entry->path_key) {
        zfree(entry);
        return NULL;
    }

    entry->cookie_list_head = zmalloc(sizeof(struct ws_cookie_list_head));
    if (!entry->cookie_list_head) {
        zfree(entry->path_key);
        zfree(entry);
        return NULL;
    }

    TAILQ_INIT(entry->cookie_list_head);

    if (!rbProbe(domain_entry->path_cookies, entry)) {
        zfree(entry->cookie_list_head);
        zfree(entry->path_key);
        zfree(entry);
        return NULL;
    }

    return entry;
}

static void insert_or_replace_cookie(struct ws_cookie_list_head *list, ws_cookie *cookie) {
    ws_cookie *existing;
    TAILQ_FOREACH(existing, list, next) {
        if (ws_strcasecmp(existing->name, cookie->name) == 0) {
            TAILQ_REMOVE(list, existing, next);
            ws_cookie_free(existing);
            break;
        }
    }
    TAILQ_INSERT_TAIL(list, cookie, next);
}

void ws_cookie_jar_parse_set_cookie_headers(ws_cookie_jar *jar,
                                             const char *request_host,
                                             const char *request_path,
                                             bool is_https,
                                             struct evkeyvalq *set_cookie_headers)
{
    if (!jar || !request_host || !request_path || !set_cookie_headers)
        return;

    struct evkeyval *header;
    TAILQ_FOREACH(header, set_cookie_headers, next) {
        ws_cookie *cookie = parse_set_cookie_string(header->value, request_host, request_path);
        if (!cookie) {
            ws_log_warn("Failed to parse Set-Cookie: %s", header->value);
            continue;
        }

        // Validate domain
        if (!validate_cookie_domain(cookie, request_host)) {
            ws_cookie_free(cookie);
            continue;
        }

        // Enforce Secure attribute
        if (cookie->secure && !is_https) {
            ws_log_warn("Dropping Secure cookie '%s' from HTTP", cookie->name);
            ws_cookie_free(cookie);
            continue;
        }

        ws_domain_cookies *domain_entry = get_or_create_domain_entry(jar, cookie->domain);
        if (!domain_entry) {
            ws_cookie_free(cookie);
            continue;
        }

        ws_path_map_item *path_entry = get_or_create_path_entry(domain_entry, cookie->path);
        if (!path_entry) {
            ws_cookie_free(cookie);
            continue;
        }

        insert_or_replace_cookie(path_entry->cookie_list_head, cookie);
    }
}

// Function to check if a request host matches a cookie's domain
// cookie_domain here is already without leading dot (e.g., "example.com")
static bool is_domain_match(const char *request_host, const char *cookie_domain) {
    if (!request_host || !cookie_domain) return false;

    size_t req_len = strlen(request_host);
    size_t cookie_len = strlen(cookie_domain);

    // Exact match (e.g., "example.com" matches "example.com")
    if (ws_strcasecmp(request_host, cookie_domain) == 0) {
        return true;
    }

    // Subdomain match: request_host must end with ".cookie_domain"
    // e.g., "www.example.com" ends with ".example.com"
    if (req_len > cookie_len &&
        request_host[req_len - cookie_len - 1] == '.' && // Check for the preceding dot
        ws_strcasecmp(request_host + req_len - cookie_len, cookie_domain) == 0) {
        return true;
    }

    return false;
}

// Function to check if a request path matches a cookie's path
static bool is_path_match(const char *request_path, const char *cookie_path) {
    if (!request_path || !cookie_path) return false;

    size_t req_len = strlen(request_path);
    size_t cookie_len = strlen(cookie_path);

    // Exact match (e.g., "/foo" matches "/foo")
    if (strcmp(request_path, cookie_path) == 0) {
        return true;
    }

    // Prefix match: request_path must start with cookie_path.
    // And if cookie_path doesn't end with '/', the character following the prefix in request_path must be '/' or it's an exact match.
    // Example: cookie_path="/foo", request_path="/foo/bar" -> Match
    // Example: cookie_path="/foo", request_path="/foobar" -> No Match
    if (req_len >= cookie_len && strncmp(request_path, cookie_path, cookie_len) == 0) {
        if (cookie_len == 0 || cookie_path[cookie_len - 1] == '/') {
            // Cookie path is empty (matches everything) or ends with '/',
            // so any path that starts with it matches.
            return true;
        }
        // Cookie path does not end with '/', so the next character in request_path must be '/'.
        // Or if request_path is exactly the length of cookie_path (exact match already handled above but safe here).
        if (request_path[cookie_len] == '/' || req_len == cookie_len) {
            return true;
        }
    }
    return false;
}


/**
 * @brief 从 Cookie Jar 中获取适用于给定主机和路径的 "Cookie" 头部字符串。
 */
char *ws_cookie_jar_get_cookie_header_string(ws_cookie_jar *jar,
                                             const char *request_host,
                                             const char *request_path,
                                             bool is_https) {
    if (!jar || !request_host || !request_path) {
        return NULL;
    }

    struct evbuffer *cookie_buffer = evbuffer_new(); // Use evbuffer to build the cookie string efficiently
    if (!cookie_buffer) {
        ws_log_error("Failed to create evbuffer for cookie header string.");
        return NULL;
    }

    time_t current_time = time(NULL); // Get current time for expiration checks

    // --- Prepare for domain matching and traversal ---
    ws_domain_cookies search_domain_item; // Temporary item for rbFind
    rbIter domain_iter;                   // Iterator for main domain_map
    rbIterInit(&domain_iter, jar->domain_map);

    // Iterate through domains in the main domain_map.
    // The libavl's rbIterFirst/rbIterNext functions give direct data pointers.
    for (ws_domain_cookies *domain_item = (ws_domain_cookies *)rbIterFirst(&domain_iter, jar->domain_map);
         domain_item != NULL;
         domain_item = (ws_domain_cookies *)rbIterNext(&domain_iter))
    {
        if (is_domain_match(request_host, domain_item->domain)) {
            // Found a matching domain. Now iterate its nested path_cookies RBTree.
            rbIter path_iter; // Iterator for nested path_cookies tree
            rbIterInit(&path_iter, domain_item->path_cookies);

            for (ws_path_map_item *path_item = (ws_path_map_item *)rbIterFirst(&path_iter, domain_item->path_cookies);
                 path_item != NULL;
                 path_item = (ws_path_map_item *)rbIterNext(&path_iter))
            {
                const char *cookie_path_key = path_item->path_key;                 // The path string
                struct ws_cookie_list_head *cookie_list = path_item->cookie_list_head; // The TAILQ of cookies

                // Check if the request path matches the cookie's path
                if (is_path_match(request_path, cookie_path_key)) {
                    ws_cookie *cookie;
                    // Iterate safely through the cookie list.
                    TAILQ_FOREACH(cookie, cookie_list, next) {
                        // Check expiration: If expires > 0 and in the past, remove and skip.
                        if (cookie->expires > 0 && cookie->expires < current_time) {
                            ws_log_debug("Expired cookie found: %s. Removing.", cookie->name);
                            TAILQ_REMOVE(cookie_list, cookie, next); // Remove from list
                            ws_cookie_free(cookie);                   // Free cookie's memory
                            continue; // Go to the next cookie in the list
                        }

                        // Check secure flag: If cookie is secure and request is HTTP, skip.
                        if (cookie->secure && !is_https) {
                            continue; // Do not send secure cookie over non-HTTPS connection
                        }

                        // Append the cookie to the buffer in "name=value" format.
                        if (evbuffer_get_length(cookie_buffer) > 0) {
                            evbuffer_add_printf(cookie_buffer, "; "); // Add separator if not the first cookie
                        }
                        evbuffer_add_printf(cookie_buffer, "%s=%s", cookie->name, cookie->value);
                    }
                }
            }
        }
    }

    // If no cookies were added to the buffer, return NULL.
    if (evbuffer_get_length(cookie_buffer) == 0) {
        evbuffer_free(cookie_buffer);
        return NULL;
    }

    // Convert the evbuffer content to a dynamically allocated string.
    char *cookie_header_str = strdup((const char *)evbuffer_pullup(cookie_buffer, -1));
    evbuffer_free(cookie_buffer); // Free the evbuffer

    return cookie_header_str;
}