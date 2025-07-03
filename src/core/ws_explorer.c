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
#include <unistd.h> // For close()
#include <string.h> // For strstr
#include <stdio.h>  // For printf, fprintf
#include <stdlib.h> // For malloc, free
#include <ctype.h>  // For isspace
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_url.h>
#include <ws_explorer.h>

// RequestNode Queue management
static void ws_queue_push(ws_explorer *explorer, ws_request *request) {
    ws_requestnode *new_node = (ws_requestnode*)zcalloc(sizeof(ws_requestnode));
    if (!new_node) {
        ws_log_error("Failed to allocate memory for request node.");
        ws_request_free(request);
        return;
    }
    new_node->request = request;
    new_node->next = NULL;

    if (explorer->queue_tail) {
        explorer->queue_tail->next = new_node;
    } else {
        explorer->queue_head = new_node;
    }
    explorer->queue_tail = new_node;
}

static ws_request* ws_queue_pop(ws_explorer *explorer) {
    if (!explorer->queue_head) {
        return NULL;
    }
    ws_requestnode *node_to_pop = explorer->queue_head;
    ws_request *request = node_to_pop->request;
    explorer->queue_head = node_to_pop->next;
    if (explorer->queue_head == NULL) {
        explorer->queue_tail = NULL;
    }
    zfree(node_to_pop);
    return request;
}

// Visited URLs management (using rbTree)
// Simple string hash function (FNV-1a)
static unsigned long long hash_string(const char *str) {
    unsigned long long hash = 14695981039346656037ULL; // FNV_OFFSET_64
    for (const char *s = str; *s; ++s) {
        hash ^= (unsigned char)*s;
        hash *= 1099511628211ULL; // FNV_PRIME_64
    }
    return hash;
}

// Compare function for string data stored in rbtree
static int url_string_compare_func(const void *a, const void *b, void *param) {
    (void)param;

    // First, compare the strings directly.
    int cmp = strcmp((const char*)a, (const char*)b);
    if (cmp != 0) {
        return cmp;
    }

    // If strings are identical, their hashes must be too.
    // This hash comparison is now redundant if strcmp is used first,
    // but we keep it to show the original logic.
    // It could be useful if you wanted to prioritize hash for some reason,
    // but direct string comparison is more robust for correctness.
    unsigned long long urla_hash = hash_string((const char*)a);
    unsigned long long urlb_hash = hash_string((const char*)b);
    return (urla_hash < urlb_hash) ? -1 : (urla_hash > urlb_hash) ? 1 : 0;
}

// Free function for string data stored in rbtree
static void url_string_free_func(void *data, void *param) {
    zfree(data);
}

bool ws_explorer_has_visited(ws_explorer *explorer, const char *url) {
    if (!explorer || !url) return false;
    // For visited check, we hash the FLD (First Level Domain) + path for simplicity.
    // Or, for stricter unique check, hash the full canonicalized URL.
    // Let's use the full canonicalized URL for visited.
    // unsigned long long url_hash = hash_string(url);
    return rbFind(explorer->visited_urls, (void*)url) != NULL;
}

void ws_explorer_mark_visited(ws_explorer *explorer, const char *url) {
    if (!explorer || !url || ws_explorer_has_visited(explorer, url)) return;

    char *url_copy = ws_safe_strdup(url);
    if (!url_copy) {
        ws_log_error("Failed to duplicate URL for visited set.");
        return;
    }

    // unsigned long long url_hash = hash_string(url_copy);
    if (rbInsert(explorer->visited_urls, (void*)url_copy) != NULL) {
        ws_log_warn("Failed to mark URL '%s' as visited (possibly duplicate hash or insert error).", url);
        zfree(url_copy); // Free if insertion failed
    }
}

// Placeholder for HTML/JS link extraction
// In a real crawler, you'd use a robust HTML parsing library (e.g., libxml2, Gumbo)
// to accurately find links in <a>, <link>, <script> tags, etc.
ws_request** ws_extract_links(ws_explorer *explorer, const ws_response *response, const ws_request *original_request, size_t *num_links_out) {
    *num_links_out = 0;
    if (!response || !response->content || original_request->link_depth >= explorer->max_depth) {
        return NULL;
    }

    // Simplified link extraction: look for "http" or "https" strings as potential URLs
    // This is EXTREMELY naive and prone to errors.
    // Real-world: use libxml2 to parse HTML, find <a> tags, extract hrefs, resolve relative URLs.
    const char *content = response->content;
    size_t content_len = response->content_len;
    ws_request **extracted_requests = NULL;
    size_t capacity = 0;
    size_t count = 0;

    const char *ptr = content;
    const char *end_ptr = content + content_len;

    while (ptr < end_ptr) {
        const char *http_pos = strstr(ptr, "http://");
        const char *https_pos = strstr(ptr, "https://");

        const char *link_start = NULL;
        if (http_pos && (!https_pos || http_pos < https_pos)) {
            link_start = http_pos;
        } else if (https_pos) {
            link_start = https_pos;
        }

        if (!link_start) {
            break; // No more "http" or "https" found
        }

        // Find the end of the URL (space, quote, <, >, etc.)
        const char *link_end = link_start;
        while (link_end < end_ptr && !isspace(*link_end) && *link_end != '"' && *link_end != '<' && *link_end != '>') {
            link_end++;
        }

        if (link_end > link_start) {
            size_t url_len = link_end - link_start;
            char *found_url = (char*)zmalloc(url_len + 1);
            if (found_url) {
                strncpy(found_url, link_start, url_len);
                found_url[url_len] = '\0';

                // Resolve URL if it's a relative path (our simple search won't find relative, but good practice)
                char *resolved_url = ws_url_resolve(response->url, found_url); // response->url is the base for newly found links

                if (resolved_url) {
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        ws_request **new_array = (ws_request**)zrealloc(extracted_requests, capacity * sizeof(ws_request*));
                        if (!new_array) {
                            ws_log_error("realloc failed for extracted_requests");
                            zfree(resolved_url);
                            zfree(found_url);
                            // Clean up already allocated requests if realloc fails
                            for (size_t i = 0; i < count; ++i) ws_request_free(extracted_requests[i]);
                            zfree(extracted_requests);
                            *num_links_out = 0;
                            return NULL;
                        }
                        extracted_requests = new_array;
                    }
                    extracted_requests[count] = ws_request_new(resolved_url, "GET", original_request->link_depth + 1,
                                                             NULL, NULL, 0, false, NULL, 0, NULL, response->url, original_request->user_data); // Pass explorer as user_data
                    if (extracted_requests[count]) {
                        count++;
                    } else {
                        ws_log_error("Failed to create new request for extracted URL: %s\n", resolved_url);
                    }
                    zfree(resolved_url);
                } else {
                    ws_log_error("Failed to resolve URL: %s\n", found_url);
                }
                zfree(found_url);
            }
        }
        ptr = link_end; // Continue search from after the found link
    }
    *num_links_out = count;
    return extracted_requests;
}

ws_explorer* ws_explorer_new(int max_depth, size_t max_page_size, int parallelism) {
    ws_explorer *explorer = (ws_explorer*)zcalloc(sizeof(ws_explorer));
    if (!explorer) return NULL;

    explorer->event_base = ws_event_new(); // Initialize your event base
    if (!explorer->event_base) { 
        zfree(explorer); 
        return NULL;
    }

    explorer->http_manager = ws_http_init(explorer->event_base); // Initialize HTTP client
    if (!explorer->http_manager) { 
        ws_event_free(explorer->event_base); 
        zfree(explorer); 
        return NULL; 
    }

    explorer->max_depth = max_depth;
    explorer->max_page_size = max_page_size;
    explorer->parallelism = parallelism;
    explorer->active_easy_handles = 0;

    // Initialize visited URLs red-black tree with string hashing
    explorer->visited_urls = rbCreate(url_string_compare_func, NULL);
    if (!explorer->visited_urls) {
        ws_http_cleanup(explorer->http_manager);
        ws_event_free(explorer->event_base);
        zfree(explorer);
        return NULL;
    }

    explorer->stop_flag = false;
    return explorer;
}

void ws_explorer_free(ws_explorer *explorer) {
    if (explorer) {
        ws_http_cleanup(explorer->http_manager); // Clean up HTTP module (including CURL handles)
        ws_event_free(explorer->event_base);     // Clean up event base

        // Free remaining requests in queue
        ws_request *req;
        while ((req = ws_queue_pop(explorer)) != NULL) {
            ws_request_free(req);
        }
        rbDestroy(explorer->visited_urls, url_string_free_func); // Free visited URLs rbtree
        explorer->visited_urls = NULL;
        zfree(explorer);
    }
}

// Helper to try adding requests from queue if parallelism limit allows
static void ws_explorer_try_add_requests(ws_explorer *explorer) {
    ws_log_info("Entering ws_explorer_try_add_requests (Active: %d)", explorer->active_easy_handles);
    while (explorer->active_easy_handles < explorer->parallelism && !explorer->stop_flag) {
        ws_request *next_req = ws_queue_pop(explorer);
        if (!next_req) {
            ws_log_info("Queue is empty, breaking from try_add_requests.");
            break; // Queue is empty
        }

        char *full_url = ws_safe_strdup(next_req->url); // Use request's URL directly for uniqueness check
        if (!full_url) {
            ws_log_error("Error: Could not get full URL for request.");
            ws_request_free(next_req);
            continue;
        }

        // Use FLD for "domain-level" visited check, then full URL for "page-level" visited check.
        // For simplicity, we just use the full canonicalized URL hash.
        if (ws_explorer_has_visited(explorer, full_url) || next_req->link_depth > explorer->max_depth) {
            ws_log_info("Skipping URL (visited or max depth exceeded): %s (Depth: %d)", full_url, next_req->link_depth);
            zfree(full_url);
            ws_request_free(next_req);
            continue;
        }

        // Add request to HTTP client
        if (ws_http_perform_request(explorer->http_manager,
                                    next_req, ws_explorer_http_completion_cb)) {
            ws_explorer_mark_visited(explorer, full_url);
            explorer->active_easy_handles++;
            ws_log_info("Adding request for URL: %s (Active: %d, Depth: %d)", full_url, explorer->active_easy_handles, next_req->link_depth);
        } else {
            ws_log_error("Failed to perform HTTP request for %s. Discarding.", full_url);
            ws_request_free(next_req); // Discard and free if failed to add
        }
        zfree(full_url);
        ws_log_info("Looping in try_add_requests (Active: %d)", explorer->active_easy_handles);
    }
    ws_log_info("Exiting ws_explorer_try_add_requests (Active: %d)", explorer->active_easy_handles);
}

// This is the callback from ws_http.c when a request completes
static void ws_explorer_http_completion_cb(int status_code, struct evkeyvalq *headers,
                                            const char *body, size_t body_len,
                                            void *user_data, int error_code) {
    ws_log_info("Entering http_completion_cb");
    // user_data here is the original ws_request that we passed to ws_http_perform_request.
    ws_request *completed_request = (ws_request *)user_data;
    ws_explorer *explorer_parent = (ws_explorer *)completed_request->user_data; // This holds the explorer instance

    if (!explorer_parent) {
        ws_log_error("Error: Explorer parent not found in completion callback for URL: %s", completed_request->url);
        ws_request_free(completed_request); // Ensure request is freed even if explorer context is lost
        return;
    }

    explorer_parent->active_easy_handles--;
    ws_log_info("Completed request for URL: %s, Status: %d, Size: %zu bytes (Active: %d)",
           completed_request->response->url ? completed_request->response->url : completed_request->url,
           status_code, completed_request->response->content_len, explorer_parent->active_easy_handles);

    // Process response: extract links
    if (status_code >= 200 && status_code < 300 && error_code == 0) { // Success
        size_t num_extracted_links = 0;
        ws_request **extracted_requests = ws_extract_links(explorer_parent, completed_request->response, completed_request, &num_extracted_links);
        for (size_t i = 0; i < num_extracted_links; ++i) {
            if (extracted_requests[i]) {
                 char *link_full_url = ws_safe_strdup(extracted_requests[i]->url); // Use request's URL directly
                 if (!link_full_url) {
                     ws_log_error("Error: Could not get full URL for extracted link.");
                     ws_request_free(extracted_requests[i]);
                     continue;
                 }

                 if (!ws_explorer_has_visited(explorer_parent, link_full_url)) {
                     ws_queue_push(explorer_parent, extracted_requests[i]);
                 } else {
                     ws_request_free(extracted_requests[i]); // Free if already visited
                 }
                 zfree(link_full_url);
            }
        }
        zfree(extracted_requests); // Free array of pointers
    } else {
        ws_log_error("Request failed for %s. Error Code: %d, Message: %s",
                completed_request->response->url ? completed_request->response->url : completed_request->url,
                error_code, completed_request->response->error_message ? completed_request->response->error_message : "No specific error message");
    }

    ws_request_free(completed_request); // Free the request and its associated response

    // Try to add more requests if parallelism allows
    ws_explorer_try_add_requests(explorer_parent);

    // Check if everything is done
    if (explorer_parent->active_easy_handles == 0 && explorer_parent->queue_head == NULL) {
        ws_log_info("All active requests completed and queue is empty. Stopping explorer.");
        ws_event_stop(explorer_parent->event_base);
    }
    ws_log_info("Exiting http_completion_cb");
}

void ws_explorer_explore(ws_explorer *explorer, ws_request *initial_request) {
    if (!explorer || !initial_request) return;

    // The user_data in ws_request will be passed back to ws_explorer_http_completion_cb.
    // So, we set it to the explorer instance itself to regain context.
    initial_request->user_data = explorer;

    ws_queue_push(explorer, initial_request);

    // Initially add requests up to parallelism limit
    ws_explorer_try_add_requests(explorer);

    // Start the event loop
    ws_event_loop(explorer->event_base);
    printf("Explorer event loop exited.\n");
}

void ws_explorer_stop(ws_explorer *explorer) {
    explorer->stop_flag = true;
    ws_event_stop(explorer->event_base);
}
