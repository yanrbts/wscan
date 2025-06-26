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
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_util.h>
#include <ws_cookie.h>

// Convert HTTP date string to time_t
// Simplified parser, might not handle all RFC date formats robustly.
// Consider using a more robust date parsing library if full RFC compliance is critical.
static time_t ws_parse_http_date(const char *date_str) {
    struct tm tm;
    // Example format: "Wed, 09 Jun 2021 10:18:14 GMT"
    // Using strptime for more robust parsing if available and desired.
    // For simplicity, a basic sscanf or custom parser for common formats.
    // A more robust solution might use libcurl's curl_getdate.
    // For this example, let's assume a common RFC1123-like format.
    // Example: "Wdy, DD Mon YYYY HH:MM:SS GMT"
    char weekday[4], month[4], tz[4];
    int day, year, hour, min, sec;
    char *s = (char*)date_str;

    // Skip weekday and comma
    while (*s && *s != ',') s++;
    if (*s == ',') s++;
    s = ws_trim_whitespace(s); // Trim space after comma

    if (sscanf(s, "%d %3s %d %d:%d:%d %3s",
               &day, month, &year, &hour, &min, &sec, tz) == 7) {
        memset(&tm, 0, sizeof(tm));
        tm.tm_mday = day;
        tm.tm_year = year - 1900;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;

        // Simplified month parsing
        if (ws_strcasecmp(month, "Jan") == 0) tm.tm_mon = 0;
        else if (ws_strcasecmp(month, "Feb") == 0) tm.tm_mon = 1;
        else if (ws_strcasecmp(month, "Mar") == 0) tm.tm_mon = 2;
        else if (ws_strcasecmp(month, "Apr") == 0) tm.tm_mon = 3;
        else if (ws_strcasecmp(month, "May") == 0) tm.tm_mon = 4;
        else if (ws_strcasecmp(month, "Jun") == 0) tm.tm_mon = 5;
        else if (ws_strcasecmp(month, "Jul") == 0) tm.tm_mon = 6;
        else if (ws_strcasecmp(month, "Aug") == 0) tm.tm_mon = 7;
        else if (ws_strcasecmp(month, "Sep") == 0) tm.tm_mon = 8;
        else if (ws_strcasecmp(month, "Oct") == 0) tm.tm_mon = 9;
        else if (ws_strcasecmp(month, "Nov") == 0) tm.tm_mon = 10;
        else if (ws_strcasecmp(month, "Dec") == 0) tm.tm_mon = 11;
        else return 0; // Invalid month

        // mktime assumes local time, we need gmtime/timegm for GMT/UTC dates.
        // POSIX timegm is ideal, otherwise manual offset.
        // For portability, we'll convert to UTC directly if possible.
        // On most systems, setenv("TZ", "UTC", 1) then mktime will work, but that's global.
        // Better: use gmtime/timegm or parse into components.
        // For simplicity: convert to seconds since epoch, assuming GMT.
        return timegm(&tm); // Requires _GNU_SOURCE or similar, or calculate manually.
                            // If timegm not available, a portable workaround is needed.
                            // For this example, assuming timegm is available or use UTC conversion logic.
                            // If timegm isn't available, cross-platform date parsing is harder.
                            // For production, consider a dedicated library or a robust hand-written parser.
    }
    return 0; // Failed to parse
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

// Parses a single "Set-Cookie" header string and returns a ws_cookie struct.
// Returns NULL on parsing error.
static ws_cookie *parse_set_cookie_string(const char *cookie_str, const char *default_domain, const char *default_path) {
    ws_cookie *new_cookie = zcalloc(sizeof(ws_cookie));
    if (!new_cookie) {
        ws_log_error("Failed to allocate memory for new cookie.");
        return NULL;
    }

    new_cookie->expires = 0; // Session cookie by default
    new_cookie->secure = false;
    new_cookie->httponly = false;
    // Duplicate default domain and path. These might be overwritten by attributes in cookie_str.
    new_cookie->domain = zstrdup(default_domain);
    new_cookie->path = zstrdup(default_path);
    if (!new_cookie->domain || !new_cookie->path) {
        ws_log_error("Failed to duplicate default domain/path for cookie.");
        ws_cookie_free(new_cookie);
        return NULL;
    }

    // Create a mutable copy of the cookie string for parsing
    char *cookie_copy = zstrdup(cookie_str);
    if (!cookie_copy) {
        ws_log_error("Failed to duplicate Set-Cookie string.");
        ws_cookie_free(new_cookie);
        return NULL;
    }

    char *token_start = cookie_copy;
    char *token_end;

    // Parse Name=Value (the first part)
    token_end = strchr(token_start, ';');
    char *name_value_pair;
    if (token_end) {
        *token_end = '\0';          // Null-terminate the name=value part
        name_value_pair = token_start;
        token_start = token_end + 1; // Move pointer to start of next attribute
    } else {
        name_value_pair = token_start;
        token_start = NULL; // No more attributes after name=value
    }

    char *eq_pos = strchr(name_value_pair, '=');
    if (!eq_pos) {
        ws_log_warn("Invalid Set-Cookie: Missing '=' in name-value pair: %s", name_value_pair);
        zfree(cookie_copy);
        ws_cookie_free(new_cookie);
        return NULL;
    }

    *eq_pos = '\0'; // Null-terminate the name part
    new_cookie->name = zstrdup(ws_trim_whitespace(name_value_pair)); // Name
    new_cookie->value = zstrdup(ws_trim_whitespace(eq_pos + 1));     // Value
    if (!new_cookie->name || !new_cookie->value) {
        ws_log_error("Failed to duplicate cookie name/value.");
        zfree(cookie_copy);
        ws_cookie_free(new_cookie);
        return NULL;
    }

    // Parse other attributes (Domain, Path, Expires, Max-Age, Secure, HttpOnly)
    while (token_start && *token_start) {
        token_start = ws_trim_whitespace(token_start); // Trim leading whitespace for the attribute
        token_end = strchr(token_start, ';');
        char *attr_pair;
        if (token_end) {
            *token_end = '\0';
            attr_pair = token_start;
            token_start = token_end + 1;
        } else {
            attr_pair = token_start;
            token_start = NULL; // Last attribute
        }

        char *attr_eq_pos = strchr(attr_pair, '=');
        char *attr_name;
        char *attr_value = NULL;
        if (attr_eq_pos) {
            *attr_eq_pos = '\0';
            attr_name = ws_trim_whitespace(attr_pair);
            attr_value = ws_trim_whitespace(attr_eq_pos + 1);
        } else {
            attr_name = ws_trim_whitespace(attr_pair); // Flag attributes like "Secure" or "HttpOnly"
        }

        if (ws_strcasecmp(attr_name, "Domain") == 0 && attr_value) {
            zfree(new_cookie->domain); // Free the default domain
            // RFC 6265, Section 4.1.2.3: if domain starts with '.', remove it for storage
            if (attr_value[0] == '.') {
                new_cookie->domain = zstrdup(attr_value + 1);
            } else {
                new_cookie->domain = zstrdup(attr_value);
            }
        } else if (ws_strcasecmp(attr_name, "Path") == 0 && attr_value) {
            zfree(new_cookie->path); // Free the default path
            new_cookie->path = zstrdup(attr_value);
        } else if (ws_strcasecmp(attr_name, "Expires") == 0 && attr_value) {
            new_cookie->expires = ws_parse_http_date(attr_value);
        } else if (ws_strcasecmp(attr_name, "Max-Age") == 0 && attr_value) {
            long long max_age = strtoll(attr_value, NULL, 10); // Parse as long long
            if (max_age >= 0) {
                new_cookie->expires = time(NULL) + max_age; // Current time + max_age
            } else {
                // Max-Age=0 (or negative) means delete cookie immediately, set to past date.
                new_cookie->expires = 1; // Any past time will work (e.g., epoch + 1 second)
            }
        } else if (ws_strcasecmp(attr_name, "Secure") == 0) {
            new_cookie->secure = true;
        } else if (ws_strcasecmp(attr_name, "HttpOnly") == 0) {
            new_cookie->httponly = true;
        }
        // Ignore other unknown attributes
    }

    zfree(cookie_copy); // Free the mutable copy
    return new_cookie;
}

/**
 * @brief 从 HTTP 响应头中解析 "Set-Cookie" 头部，并添加到 Cookie Jar。
 */
void ws_cookie_jar_parse_set_cookie_headers(ws_cookie_jar *jar,
                                             const char *request_host,
                                             const char *request_path,
                                             bool is_https,
                                             struct evkeyvalq *set_cookie_headers) {
    if (!jar || !request_host || !request_path || !set_cookie_headers) {
        return;
    }

    struct evkeyval *set_cookie_header;
    // Iterate through each "Set-Cookie" header in the response
    TAILQ_FOREACH(set_cookie_header, set_cookie_headers, next) {
        // Parse the raw Set-Cookie string into a ws_cookie struct
        ws_cookie *new_cookie = parse_set_cookie_string(set_cookie_header->value, request_host, request_path);
        if (!new_cookie) {
            ws_log_warn("Failed to parse Set-Cookie header: %s", set_cookie_header->value);
            continue;
        }

        // --- Apply Cookie Rules (RFC 6265, Section 5.3) ---

        // 1. Domain Validation:
        //    If the Domain attribute is present:
        //    - The request-host must "domain-match" the Domain attribute.
        //    - If it's a "public suffix" (e.g., .com, .co.uk), it's more complex, but we'll use a simplified check.
        //    - Our stored domain doesn't have a leading dot, so "example.com" should match "example.com" and "www.example.com".
        if (new_cookie->domain) {
            size_t req_host_len = strlen(request_host);
            size_t cookie_domain_len = strlen(new_cookie->domain);

            // Basic check: request host must be at least as long as cookie domain
            if (req_host_len < cookie_domain_len) {
                ws_log_warn("Set-Cookie domain '%s' too long for request host '%s'. Dropping.",
                            new_cookie->domain, request_host);
                ws_cookie_free(new_cookie);
                continue;
            }

            // Check for exact match or subdomain match (e.g., "www.example.com" vs "example.com")
            // A subdomain match requires the request host to end with ".cookie_domain" or directly match.
            // Example: request_host="www.example.com", new_cookie->domain="example.com"
            // req_host_len - cookie_domain_len = 15 - 11 = 4
            // request_host[4 - 1] = request_host[3] = '.'
            // strcasecmp("example.com", "example.com") == 0
            if (ws_strcasecmp(request_host, new_cookie->domain) != 0 && // Not an exact match
                !(req_host_len > cookie_domain_len &&                   // Is longer
                  request_host[req_host_len - cookie_domain_len - 1] == '.' && // Preceded by a dot
                  ws_strcasecmp(request_host + req_len - cookie_domain_len, new_cookie->domain) == 0)) {
                ws_log_warn("Set-Cookie domain '%s' does not match request host '%s'. Dropping.",
                            new_cookie->domain, request_host);
                ws_cookie_free(new_cookie);
                continue;
            }
        } else {
            // If Domain attribute is NOT specified, it defaults to the request-host.
            // The request-host is always a valid domain for the cookie.
            zfree(new_cookie->domain); // Free the default domain duplicated in parse_set_cookie_string
            new_cookie->domain = zstrdup(request_host); // Set to the actual request host
            if (!new_cookie->domain) {
                ws_log_error("Failed to duplicate request host for cookie domain.");
                ws_cookie_free(new_cookie);
                continue;
            }
        }

        // 2. Secure Flag Validation:
        //    If the Secure attribute is present, the cookie MUST NOT be stored if the request was made over HTTP.
        if (new_cookie->secure && !is_https) {
            ws_log_warn("Received Secure cookie '%s' over HTTP. Dropping.", new_cookie->name);
            ws_cookie_free(new_cookie);
            continue;
        }

        ws_log_debug("Storing cookie: %s=%s; Domain=%s; Path=%s; Expires=%ld; Secure=%d; HttpOnly=%d",
                     new_cookie->name, new_cookie->value, new_cookie->domain, new_cookie->path,
                     new_cookie->expires, new_cookie->secure, new_cookie->httponly);

        // --- Store the cookie in the Cookie Jar (RBTree nested structure) ---

        // Step A: Find or create the domain entry in jar->domain_map
        // Create a temporary search item for the domain RBTree lookup.
        ws_domain_cookies search_domain_item;
        search_domain_item.domain = new_cookie->domain; // Point to new_cookie's domain for comparison

        ws_domain_cookies *domain_entry = (ws_domain_cookies *)rbFind(jar->domain_map, &search_domain_item);

        if (!domain_entry) {
            // Domain entry does not exist, so create a new one.
            domain_entry = zmalloc(sizeof(ws_domain_cookies));
            if (!domain_entry) {
                ws_log_error("Failed to allocate ws_domain_cookies.");
                ws_cookie_free(new_cookie);
                continue;
            }
            domain_entry->domain = zstrdup(new_cookie->domain); // Duplicate domain string for storage
            if (!domain_entry->domain) {
                ws_log_error("Failed to duplicate domain string for new domain entry.");
                zfree(domain_entry);
                ws_cookie_free(new_cookie);
                continue;
            }
            // Create the nested path_cookies RBTree for this new domain entry.
            // It uses ws_cookie_path_item_cmp for comparison and ws_cookie_path_item_free for item cleanup.
            domain_entry->path_cookies = rbCreate(ws_cookie_path_item_cmp, NULL, ws_cookie_path_item_free);
            if (!domain_entry->path_cookies) {
                ws_log_error("Failed to create path_cookies RBTree for new domain.");
                zfree(domain_entry->domain);
                zfree(domain_entry);
                ws_cookie_free(new_cookie);
                continue;
            }

            // Probe (insert) the new domain_entry into the main domain_map RBTree.
            // rbProbe returns a void** to the location where the item should be placed.
            void **probe_res_ptr = rbProbe(jar->domain_map, domain_entry);
            if (!probe_res_ptr) { // rbProbe should not fail if memory is available and item is unique.
                ws_log_error("Failed to probe new domain_cookies into map.");
                // Clean up the newly allocated domain_entry and its nested tree
                rbDestroy(domain_entry->path_cookies, NULL);
                zfree(domain_entry->domain);
                zfree(domain_entry);
                ws_cookie_free(new_cookie);
                continue;
            }
            // If the item was new, *probe_res_ptr will be NULL, and we assign it.
            // If it was already present, *probe_res_ptr would point to the existing item,
            // but our logic ensures we only call probe if rbFind returned NULL.
            *probe_res_ptr = domain_entry; // Store the newly created domain_entry
        } // If domain_entry already exists, we simply proceed using it.

        // Step B: Find or create the path entry within the domain's path_cookies RBTree
        // Create a temporary search item for the path RBTree lookup.
        ws_path_map_item search_path_item;
        search_path_item.path_key = new_cookie->path; // Point to new_cookie's path for comparison

        ws_path_map_item *path_entry = (ws_path_map_item *)rbFind(domain_entry->path_cookies, &search_path_item);

        struct ws_cookie_list_head *cookie_list;
        if (!path_entry) {
            // Path entry does not exist, create a new one.
            path_entry = zmalloc(sizeof(ws_path_map_item));
            if (!path_entry) {
                ws_log_error("Failed to allocate ws_path_map_item.");
                ws_cookie_free(new_cookie);
                continue;
            }
            path_entry->path_key = zstrdup(new_cookie->path); // Duplicate path string for storage
            if (!path_entry->path_key) {
                ws_log_error("Failed to duplicate path string for new path entry.");
                zfree(path_entry);
                ws_cookie_free(new_cookie);
                continue;
            }
            // Allocate and initialize the TAILQ head for this path.
            cookie_list = zmalloc(sizeof(struct ws_cookie_list_head));
            if (!cookie_list) {
                ws_log_error("Failed to allocate cookie_list_head.");
                zfree(path_entry->path_key);
                zfree(path_entry);
                ws_cookie_free(new_cookie);
                continue;
            }
            TAILQ_INIT(cookie_list); // Initialize the TAILQ
            path_entry->cookie_list_head = cookie_list;

            // Probe (insert) the new path_entry into the domain's path_cookies RBTree.
            void **path_probe_res_ptr = rbProbe(domain_entry->path_cookies, path_entry);
            if (!path_probe_res_ptr) {
                 ws_log_error("Failed to probe new path_map_item into path map.");
                 zfree(path_entry->cookie_list_head);
                 zfree(path_entry->path_key);
                 zfree(path_entry);
                 ws_cookie_free(new_cookie);
                 continue;
            }
            *path_probe_res_ptr = path_entry; // Store the newly created path_entry
        } else {
            cookie_list = path_entry->cookie_list_head; // Use the existing TAILQ
        }

        // Step C: Add/Replace the new_cookie in the appropriate cookie_list (TAILQ)
        ws_cookie *existing_cookie;
        bool replaced = false;
        // Iterate safely to find and replace if a cookie with the same name exists for this domain/path.
        TAILQ_FOREACH(existing_cookie, cookie_list, next) {
            if (ws_strcasecmp(existing_cookie->name, new_cookie->name) == 0) {
                ws_log_debug("Replacing existing cookie '%s' for Domain=%s, Path=%s.",
                             existing_cookie->name, existing_cookie->domain, existing_cookie->path);
                TAILQ_REMOVE(cookie_list, existing_cookie, next); // Remove the old cookie
                ws_cookie_free(existing_cookie);                   // Free its memory
                TAILQ_INSERT_TAIL(cookie_list, new_cookie, next); // Add the new cookie
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // No existing cookie with the same name, simply add the new cookie to the list.
            TAILQ_INSERT_TAIL(cookie_list, new_cookie, next);
        }
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
    char *cookie_header_str = zstrdup((const char *)evbuffer_pullup(cookie_buffer, -1));
    evbuffer_free(cookie_buffer); // Free the evbuffer

    return cookie_header_str;
}