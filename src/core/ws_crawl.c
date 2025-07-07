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
 * ARISING IN ANY WAY OUT OF THE USE of THIS SOFTWARE, EVEN IF ADVISED of THE
 * POSSIBILITY of SUCH DAMAGE.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // For snprintf
#include <stdbool.h> // For bool
#include <ws_malloc.h> 
#include <ws_log.h>
#include <ws_crawl.h>

typedef struct ws_buffer {
    char *buf;
    size_t len;
    size_t cap;
} ws_buffer_t;

typedef struct ws_url_node {
    char *url;
    struct ws_url_node *next;
} ws_url_node_t;

// Internal structure for a crawling task (passed as user_data to ws_http)
typedef struct ws_crawl_task {
    ws_crawler_t *crawler;
    char *url;
    ws_buffer_t *content_buffer; // Buffer to accumulate response body
} ws_crawl_task_t;

struct ws_crawler {
    ws_http_client_t *http_client;
    ws_event_loop_t *event_loop;
    int max_concurrent_requests;
    int active_requests;

    // URL Queue
    ws_url_node_t *url_queue_head;
    ws_url_node_t *url_queue_tail;
    size_t url_queue_size;

    // Set for visited URLs (simple hash set replacement for now)
    // For a real crawler, use a proper hash set or bloom filter
    char **visited_urls;
    size_t visited_urls_count;
    size_t visited_urls_capacity;
    
    ws_crawl_page_cb_fn page_callback;
    ws_crawl_error_cb_fn error_callback;
    void *user_data; // User data for crawler callbacks

    // Timer to periodically check/dispatch new requests from queue
    ws_event_t *dispatch_timer;
};

/**
 * @brief Initializes a dynamic buffer.
 * @return Pointer to a new ws_buffer_t, or NULL on failure.
 */
static ws_buffer_t *ws_buffer_new() {
    ws_buffer_t *buf = zcalloc(sizeof(ws_buffer_t));
    if (!buf) {
        ws_log_error("Failed to allocate ws_buffer_t");
        return NULL;
    }
    buf->cap = 1024; // Initial capacity
    buf->buf = zmalloc(buf->cap);
    if (!buf->buf) {
        ws_log_error("Failed to allocate initial buffer for ws_buffer_t");
        zfree(buf);
        return NULL;
    }
    buf->len = 0;
    return buf;
}

/**
 * @brief Appends data to the dynamic buffer.
 * @param buf The buffer to append to.
 * @param data The data to append.
 * @param data_len The length of the data.
 * @return true on success, false on failure (e.g., out of memory).
 */
static bool ws_buffer_append(ws_buffer_t *buf, const char *data, size_t data_len) {
    if (!buf) return false;

    if (buf->len + data_len > buf->cap) {
        size_t new_cap = buf->cap;
        while (new_cap < buf->len + data_len) {
            new_cap *= 2; // Double capacity
        }
        char *new_buf = zrealloc(buf->buf, new_cap);
        if (!new_buf) {
            ws_log_error("Failed to reallocate buffer. Needed %zu, current %zu", buf->len + data_len, buf->cap);
            return false;
        }
        buf->buf = new_buf;
        buf->cap = new_cap;
    }

    memcpy(buf->buf + buf->len, data, data_len);
    buf->len += data_len;
    buf->buf[buf->len] = '\0'; // Null-terminate for string compatibility
    return true;
}

/**
 * @brief Frees a dynamic buffer.
 * @param buf The buffer to free.
 */
static void ws_buffer_free(ws_buffer_t *buf) {
    if (!buf) return;
    zfree(buf->buf);
    zfree(buf);
}

/**
 * @brief Frees a URL queue node.
 * @param node The node to free.
 */
static void ws_url_node_free(ws_url_node_t *node) {
    if (!node) return;
    zfree(node->url);
    zfree(node);
}

/**
 * @brief Checks if a URL has been visited. (Naive implementation)
 * For a real crawler, use a proper hash set for efficiency.
 * @param crawler The crawler instance.
 * @param url The URL to check.
 * @return true if visited, false otherwise.
 */
static bool ws_crawler_is_visited(ws_crawler_t *crawler, const char *url) {
    // This is a very inefficient linear scan.
    // Replace with a hash set (e.g., based on dict.c from Redis or a c-hash library)
    // or a Bloom filter for real-world scenarios.
    for (size_t i = 0; i < crawler->visited_urls_count; ++i) {
        if (strcmp(crawler->visited_urls[i], url) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Marks a URL as visited. (Naive implementation)
 * For a real crawler, use a proper hash set for efficiency.
 * @param crawler The crawler instance.
 * @param url The URL to mark.
 * @return true on success, false on failure.
 */
static bool ws_crawler_mark_visited(ws_crawler_t *crawler, const char *url) {
    if (crawler->visited_urls_count == crawler->visited_urls_capacity) {
        size_t new_cap = (crawler->visited_urls_capacity == 0) ? 16 : crawler->visited_urls_capacity * 2;
        char **new_visited_urls = zrealloc(crawler->visited_urls, sizeof(char *) * new_cap);
        if (!new_visited_urls) {
            ws_log_error("Failed to reallocate visited_urls array.");
            return false;
        }
        crawler->visited_urls = new_visited_urls;
        crawler->visited_urls_capacity = new_cap;
    }

    crawler->visited_urls[crawler->visited_urls_count] = strdup(url);
    if (!crawler->visited_urls[crawler->visited_urls_count]) {
        ws_log_error("Failed to duplicate URL for visited set.");
        return false;
    }
    crawler->visited_urls_count++;
    return true;
}

/**
 * @brief Parses HTML content to find and add new URLs. (Naive implementation)
 * This is a very simplistic link extractor.
 * Replace with a proper HTML parser for production.
 * @param crawler The crawler instance.
 * @param base_url The base URL for resolving relative links.
 * @param html_content The HTML content to parse.
 */
static void ws_crawler_extract_links(ws_crawler_t *crawler, const char *base_url, const char *html_content) {
    if (!html_content || !base_url) return;

    const char *ptr = html_content;
    const char *href_start;
    const char *href_end;
    char found_url[2048]; // Max URL length for simplicity

    while ((href_start = strstr(ptr, "href=\"")) != NULL) {
        href_start += strlen("href=\""); // Move past "href=\""
        href_end = strchr(href_start, '"'); // Find closing quote
        if (href_end) {
            size_t len = href_end - href_start;
            if (len > 0 && len < sizeof(found_url) - 1) {
                strncpy(found_url, href_start, len);
                found_url[len] = '\0';

                // Basic URL resolution (very naive)
                // For relative URLs, you'd prepend the base_url
                // For actual production, use a proper URL parsing library.
                if ((strstr(found_url, "http://") == found_url || strstr(found_url, "https://") == found_url) && !strchr(found_url, ' ')) {
                    // It's an absolute URL, add it
                    ws_crawler_add_url(crawler, found_url);
                } else if (found_url[0] == '/' && found_url[1] != '/') { 
                    // Simple relative path like /path
                    // Construct absolute URL (very basic)
                    // Find the host part of the base_url
                    const char *proto_end = strstr(base_url, "://");
                    if (proto_end) {
                        proto_end += 3;
                        const char *host_end = strchr(proto_end, '/');
                        if (host_end) {
                            char absolute_url[2048];
                            int n = snprintf(absolute_url, sizeof(absolute_url), "%.*s%s",
                                     (int)(host_end - base_url), base_url, found_url);
                            if (n < 0 || n >= (int)sizeof(absolute_url)) {
                                ws_log_warn("absolute_url truncated or snprintf error: %d", n);
                                absolute_url[sizeof(absolute_url) - 1] = '\0';
                            }
                            ws_crawler_add_url(crawler, absolute_url);
                        }
                    }
                }
            }
            ptr = href_end + 1; // Move past this link
        } else {
            break; // No closing quote found, stop parsing
        }
    }
}

/**
 * @brief Frees a crawling task and its associated resources.
 * @param task The task to free.
 */
static void ws_crawl_task_free(ws_crawl_task_t *task) {
    if (!task) return;
    if (task->content_buffer)
        ws_buffer_free(task->content_buffer);
    if (task->url) zfree(task->url);
    zfree(task);
}

/**
 * @brief ws_http header callback for crawl tasks.
 */
static void ws_crawl_http_header_cb(const char *header, void *userdata) {
    // ws_crawl doesn't typically need to process headers specifically
    // unless for redirects, cookies, etc. which are handled by libcurl automatically
    (void)header;
    (void)userdata;
}

/**
 * @brief ws_http data callback for crawl tasks. Appends data to task buffer.
 */
static void ws_crawl_http_data_cb(const char *ptr, size_t size, void *userdata) {
    ws_crawl_task_t *task = (ws_crawl_task_t *)userdata;
    if (!task || !task->content_buffer) {
        ws_log_error("Invalid task or content buffer in data callback.");
        return; // Indicate error to libcurl
    }
    if (!ws_buffer_append(task->content_buffer, ptr, size)) {
        ws_log_error("Failed to append data to buffer for URL: %s", task->url);
        return; // Indicate error
    }
}

/**
 * @brief ws_http complete callback for crawl tasks. Handles request completion.
 */
static void ws_crawl_http_complete_cb(ws_http_request_t *req, long http_code, CURLcode curl_code, void *userdata) {
    (void)req; // req is already processed/freed by ws_http

    ws_crawl_task_t *task = (ws_crawl_task_t *)userdata;
    if (!task || !task->crawler) {
        ws_log_error("Invalid task or crawler in complete callback.");
        return;
    }

    ws_crawler_t *crawler = task->crawler;
    // Decrement active request count
    crawler->active_requests--; 

    if (curl_code == CURLE_OK) {
        if (http_code >= 200 && http_code < 300) {
            ws_log_info("Successfully crawled URL: %s (HTTP %ld)", task->url, http_code);
            if (crawler->page_callback) {
                // Ensure content is null-terminated for string compatibility if needed by user
                if (task->content_buffer->buf) task->content_buffer->buf[task->content_buffer->len] = '\0';
                crawler->page_callback(crawler, task->url, http_code,
                                        task->content_buffer->buf, task->content_buffer->len,
                                        crawler->user_data);
            }
            // Extract links from the crawled page
            ws_crawler_extract_links(crawler, task->url, task->content_buffer->buf);
        } else {
            ws_log_warn("URL %s returned HTTP error code: %ld", task->url, http_code);
            if (crawler->error_callback) {
                crawler->error_callback(crawler, task->url, curl_code, crawler->user_data);
            }
        }
    } else {
        ws_log_error("Failed to crawl URL: %s (Curl error: %d - %s)", task->url, curl_code, curl_easy_strerror(curl_code));
        if (crawler->error_callback) {
            crawler->error_callback(crawler, task->url, curl_code, crawler->user_data);
        }
    }

    // Free the task resources
    ws_crawl_task_free(task);
    
    // Dispatch next requests if available and slots are open
    if (crawler->dispatch_timer) {
        ws_event_add(crawler->dispatch_timer); // Re-arm timer to check queue
    } else {
        ws_log_debug("No dispatch timer set. Crawler might stall if queue is not drained.");
    }
    // Also call directly to ensure immediate dispatch if possible
    // s_crawler_dispatch_requests(crawler); // Potentially dispatch immediately
}

/**
 * @brief Attempts to dispatch new requests from the URL queue if slots are available.
 */
static void s_crawler_dispatch_requests(void *user_data) {
    ws_crawler_t *crawler = (ws_crawler_t *)user_data;

    while (crawler->active_requests < crawler->max_concurrent_requests &&
           crawler->url_queue_head != NULL) {
        
        ws_url_node_t *node = crawler->url_queue_head;
        char *url_to_crawl = node->url;

        // Pop from queue
        crawler->url_queue_head = node->next;
        if (crawler->url_queue_head == NULL) {
            crawler->url_queue_tail = NULL;
        }
        crawler->url_queue_size--;

        if (ws_crawler_is_visited(crawler, url_to_crawl)) {
            ws_url_node_free(node); // Free URL node
            continue; // Get next URL
        }

        if (!ws_crawler_mark_visited(crawler, url_to_crawl)) {
            ws_log_error("Failed to mark URL as visited, skipping: %s", url_to_crawl);
            ws_url_node_free(node);
            continue;
        }

        ws_crawl_task_t *task = zcalloc(sizeof(ws_crawl_task_t));
        if (!task) {
            ws_log_error("Failed to allocate crawl task for URL: %s", url_to_crawl);
            ws_url_node_free(node);
            continue;
        }
        task->crawler = crawler;
        task->url = strdup(url_to_crawl);

        // Free the queue node after URL ownership transfer
        ws_url_node_free(node); 

        task->content_buffer = ws_buffer_new();
        if (!task->content_buffer) {
            ws_log_error("Failed to create content buffer for URL: %s", url_to_crawl);
            ws_crawl_task_free(task);
            continue;
        }

        ws_log_debug("Dispatching request for URL: %s", task->url);
        // Note: ws_http_get takes ownership of task. It will be freed in ws_crawl_http_complete_cb
        if (!ws_http_get(crawler->http_client, task->url,
                         ws_crawl_http_header_cb,
                         ws_crawl_http_data_cb,
                         ws_crawl_http_complete_cb, task)) {
            ws_log_error("Failed to start HTTP GET for URL: %s", task->url);
            // Free task if HTTP request failed to start
            ws_crawl_task_free(task); 
            continue;
        }
        
        crawler->active_requests++;
        ws_log_debug("Active requests: %d, URLs in queue: %zu", crawler->active_requests, crawler->url_queue_size);
    }

    // If no more active requests and no more URLs in queue, crawling might be done.
    if (crawler->active_requests == 0 && crawler->url_queue_size == 0) {
        ws_log_info("Crawler finished all pending tasks.");
        // Potentially signal completion to the user or stop event loop
        // For now, we'll just let the event loop continue to run until explicitly stopped.
    }
}

ws_crawler_t *ws_crawler_new(ws_event_loop_t *event_loop,
                             int max_concurrent_requests,
                             ws_crawl_page_cb_fn page_callback,
                             ws_crawl_error_cb_fn error_callback,
                             void *user_data) {
    if (!event_loop || max_concurrent_requests <= 0 || !page_callback) {
        ws_log_error("Invalid arguments for ws_crawler_new.");
        return NULL;
    }

    ws_crawler_t *crawler = zcalloc(sizeof(ws_crawler_t));
    if (!crawler) {
        ws_log_error("Failed to allocate ws_crawler_t.");
        return NULL;
    }

    crawler->event_loop = event_loop;
    crawler->max_concurrent_requests = max_concurrent_requests;
    crawler->page_callback = page_callback;
    crawler->error_callback = error_callback;
    crawler->user_data = user_data;
    crawler->active_requests = 0;
    crawler->url_queue_head = NULL;
    crawler->url_queue_tail = NULL;
    crawler->url_queue_size = 0;
    crawler->visited_urls_count = 0;
    crawler->visited_urls_capacity = 0;
    crawler->visited_urls = NULL; // Will be allocated on first mark_visited

    crawler->http_client = ws_http_client_new(event_loop);
    if (!crawler->http_client) {
        ws_log_error("Failed to create ws_http_client for crawler.");
        ws_crawler_free(crawler);
        return NULL;
    }

    /* Set up a periodic timer to dispatch requests from the queue
     * This timer will be re-armed every time a request completes or a new URL is added.
     * It acts as a trigger to check for available slots and pending URLs. */
    crawler->dispatch_timer = ws_event_new_timer(event_loop, 10, false, s_crawler_dispatch_requests, crawler);
    if (!crawler->dispatch_timer) {
        ws_log_error("Failed to create dispatch timer for crawler.");
        ws_crawler_free(crawler);
        return NULL;
    }

    ws_log_info("Crawler created with max_concurrent_requests: %d", max_concurrent_requests);
    return crawler;
}

bool ws_crawler_add_url(ws_crawler_t *crawler, const char *url) {
    if (!crawler || !url || strlen(url) == 0) {
        ws_log_warn("Attempted to add NULL or empty URL.");
        return false;
    }

    /* Basic deduplication before adding to queue.
     * A proper hash set for `visited_urls` would be more efficient here.*/
    if (ws_crawler_is_visited(crawler, url)) {
        return false;
    }

    /* For URLs found during crawling, we might not mark them visited yet
     * because they are just queued. They will be marked visited when they are
     * actually dispatched. For *initial* seeds, they are marked when added. */
    ws_url_node_t *new_node = zcalloc(sizeof(ws_url_node_t));
    if (!new_node) {
        ws_log_error("Failed to allocate URL queue node.");
        return false;
    }
    new_node->url = strdup(url);
    if (!new_node->url) {
        ws_log_error("Failed to duplicate URL string for queue: %s", url);
        zfree(new_node);
        return false;
    }
    new_node->next = NULL;

    if (crawler->url_queue_tail == NULL) {
        crawler->url_queue_head = new_node;
        crawler->url_queue_tail = new_node;
    } else {
        crawler->url_queue_tail->next = new_node;
        crawler->url_queue_tail = new_node;
    }
    crawler->url_queue_size++;
    ws_log_debug("%s (Queue size: %zu)", url, crawler->url_queue_size);

    /* If there are free slots, immediately attempt to dispatch */
    if (crawler->active_requests < crawler->max_concurrent_requests) {
        // Re-arm timer to check queue
        ws_event_add(crawler->dispatch_timer); 
    }
    return true;
}

void ws_crawler_start(ws_crawler_t *crawler) {
    if (!crawler) {
        ws_log_error("Cannot start a NULL crawler.");
        return;
    }
    if (crawler->url_queue_size == 0 && crawler->active_requests == 0) {
        ws_log_warn("Starting crawler with an empty queue and no active requests. Nothing to do.");
        return;
    }
    ws_log_info("Crawler starting. Initializing dispatch...");
    // Immediately attempt to dispatch any initial URLs in the queue
    ws_event_add(crawler->dispatch_timer);
}


void ws_crawler_free(ws_crawler_t *crawler) {
    if (!crawler) return;

    ws_log_info("Freeing crawler resources...");

    // Free all URLs in the queue
    ws_url_node_t *current = crawler->url_queue_head;
    while (current) {
        ws_url_node_t *next = current->next;
        ws_url_node_free(current);
        current = next;
    }
    crawler->url_queue_head = NULL;
    crawler->url_queue_tail = NULL;
    crawler->url_queue_size = 0;

    // Free visited URLs list
    for (size_t i = 0; i < crawler->visited_urls_count; ++i) {
        zfree(crawler->visited_urls[i]);
    }
    zfree(crawler->visited_urls);
    crawler->visited_urls = NULL;
    crawler->visited_urls_count = 0;
    crawler->visited_urls_capacity = 0;

    // Free the dispatch timer event
    if (crawler->dispatch_timer) {
        ws_event_del(crawler->dispatch_timer);
        ws_event_free(crawler->dispatch_timer);
        crawler->dispatch_timer = NULL;
    }

    // Free the underlying HTTP client (this will also cancel and free pending HTTP requests)
    if (crawler->http_client) {
        ws_http_client_free(crawler->http_client);
        crawler->http_client = NULL;
    }

    zfree(crawler);
    ws_log_info("Crawler freed successfully.");
}