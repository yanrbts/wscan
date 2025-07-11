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
#include <ctype.h> // For isspace, tolower
#include <curl/curl.h>
#include <ws_malloc.h> 
#include <ws_log.h>
#include <ws_util.h>
#include <ws_crawl.h>
#include <ws_extract.h>

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
    ws_buffer_t *content_buffer;    // Buffer to accumulate response body
    char *content_type;             // To store the Content-Type header value
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

/* Helper function to trim leading and trailing whitespace from a string.
 * Returns a newly allocated string, which must be freed by the caller. */
static char* trim_whitespace(const char *str) {
    if (!str) {
        return NULL;
    }
    size_t len = strlen(str);
    if (len == 0) {
        return strdup(""); /* Return empty string for empty input */
    }

    /* Find start of non-whitespace */
    const char *end = str + len - 1;
    const char *start = str;
    while (isspace((unsigned char)*start) && start <= end) {
        start++;
    }

    /* Find end of non-whitespace */
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }

    /* Calculate trimmed length */
    len = (end < start) ? 0 : (end - start + 1);

    /* Allocate and copy the trimmed string */
    char *trimmed_str = (char *)zmalloc(len + 1);
    if (!trimmed_str) {
        ws_log_error("Failed to allocate memory for trimmed string.");
        return NULL;
    }
    memcpy(trimmed_str, start, len);
    trimmed_str[len] = '\0';

    return trimmed_str;
}

/**
 * @brief Handles the processing of extracted links from a crawled page.
 * This function is responsible for normalizing, filtering, and adding
 * new URLs to the crawler's queue.
 * @param crawler The crawler instance.
 * @param base_url The base URL of the page where links were extracted.
 * @param links_data The structure containing the extracted links.
 */
static void ws_crawler_process_curl_extracted_links(ws_crawler_t *crawler, const char *base_url, extracted_links_t *links_data) {
    if (!crawler || !base_url || !links_data) {
        ws_log_error("Invalid arguments to ws_crawler_process_extracted_links.");
        return;
    }

    CURLU *url_handle = NULL; 
    /* 1. Create the CURLU handle */
    url_handle = curl_url();
    if (!url_handle) {
        ws_log_error("Failed to create CURLU handle. Out of memory?");
        return;
    }

    for (size_t i = 0; i < links_data->count; i++) {
        const char *extracted_link_raw = links_data->links[i];
        char *full_resolved_url = NULL; /* To store the final absolute URL */
        CURLUcode uc;

        if (!extracted_link_raw || strlen(extracted_link_raw) == 0) {
            ws_log_debug("Skipping empty extracted link.");
            continue;
        }

        /* Core logic: Set base_url first, then resolve extracted_link
         * This step is crucial. It resets the url_handle's context to the original base_url, 
         * ensuring that subsequent relative link resolutions are correct. */
        uc = curl_url_set(url_handle, CURLUPART_URL, base_url, CURLU_NON_SUPPORT_SCHEME);
        if (uc != CURLUE_OK) {
            ws_log_error(
                "CRITICAL: Failed to reset CURLU handle with base URL '%s': %s. Subsequent resolutions might be incorrect.", 
                base_url, 
                curl_url_strerror(uc)
            );
            break; 
        }

        /* Set the extracted_link_trimmed into url_handle.
         * CURLU will automatically resolve it relative to the URL currently held by url_handle (i.e., base_url).
         * IMPORTANT: Do NOT pre-encode the entire URL here. CURLU expects raw URL components.
         * It will handle the encoding of problematic characters (like Chinese characters) internally.
         * CURLU_DEFAULT_SCHEME: If extracted_link is //domain/path, it will use the scheme from base_url.
         * CURLU_GUESS_SCHEME: If extracted_link is domain/path (no scheme), it will try to guess the scheme (usually http://).
         * CURLU_NON_SUPPORT_SCHEME: Allows parsing non-HTTP/HTTPS schemes like mailto:. */
        uc = curl_url_set(url_handle, CURLUPART_URL, extracted_link_raw,
                          CURLU_DEFAULT_SCHEME | CURLU_GUESS_SCHEME | CURLU_PATH_AS_IS 
                          | CURLU_NON_SUPPORT_SCHEME | CURLU_ALLOW_SPACE | CURLU_URLENCODE);
        
        if (uc != CURLUE_OK) {
            ws_log_warn("Failed to set extracted link '%s' into CURLU handle for resolution (base: '%s'): %s",
                        extracted_link_raw, base_url, curl_url_strerror(uc));
            continue; 
        }

        /* 4. Get the full resolved URL string from the handle */
        uc = curl_url_get(url_handle, CURLUPART_URL, &full_resolved_url, 0);
        if (uc != CURLUE_OK || !full_resolved_url) {
            ws_log_warn("Failed to get full resolved URL for '%s' (base: '%s'): %s",
                        extracted_link_raw, base_url, curl_url_strerror(uc));
            continue;
        }

        /* 5. Add the resolved absolute URL to the crawler queue */
        /* Log the raw link for debugging, but add the resolved one. */
        // ws_log_debug("Resolved link: Original '%s' -> Absolute '%s'", extracted_link_raw, full_resolved_url);
        ws_crawler_add_url(crawler, full_resolved_url);

        /* 6. Free the string returned by curl_url_get */
        curl_free(full_resolved_url);
        full_resolved_url = NULL;

    }

    /* Clean up the CURLU handle at the end of the function */
    if (url_handle) {
        curl_url_cleanup(url_handle);
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
    if (task->content_type) zfree(task->content_type);
    zfree(task);
}

/**
 * @brief ws_http header callback for crawl tasks.
 */
static void ws_crawl_http_header_cb(const char *header, void *userdata) {
    ws_crawl_task_t *task = (ws_crawl_task_t *)userdata;
    if (!task) {
        ws_log_warn("Header callback: Invalid task userdata.");
        return;
    }

    const char *content_type_prefix = "Content-Type: ";
    size_t prefix_len = strlen(content_type_prefix);

    if (ws_strcheck_prefix(header, content_type_prefix)) {
        const char *value_start = header + prefix_len;
        // Find end of line (CRLF) and null-terminate the value
        char *crlf = strstr(value_start, "\r\n");
        size_t value_len = crlf ? (size_t)(crlf - value_start) : strlen(value_start);

        // Free previous content_type if any
        if (task->content_type) {
            zfree(task->content_type);
        }
        
        task->content_type = zmalloc(value_len + 1);
        if (task->content_type) {
            memcpy(task->content_type, value_start, value_len);
            task->content_type[value_len] = '\0';
        } else {
            ws_log_error("Failed to allocate memory for content type.");
        }
    }
}

/**
 * @brief ws_http data callback for crawl tasks. Appends data to task buffer.
 */
static void ws_crawl_http_data_cb(const char *ptr, size_t size, void *userdata) {
    ws_crawl_task_t *task = (ws_crawl_task_t *)userdata;

    if (!task || !task->content_buffer) {
        ws_log_error("Invalid task or content buffer in data callback.");
        return;
    }

    if (!ws_buffer_append(task->content_buffer, ptr, size)) {
        ws_log_error("Failed to append data to buffer for URL: %s", task->url);
        return;
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
                if (task->content_buffer->buf) 
                    task->content_buffer->buf[task->content_buffer->len] = '\0';
                
                crawler->page_callback(crawler, task->url, http_code,
                                        task->content_buffer->buf, task->content_buffer->len,
                                        crawler->user_data);
            }

            const char *content_type = task->content_type ? task->content_type : "application/octet-stream";

            // Extract links from the crawled page
            if (task->content_buffer->buf && task->content_buffer->len > 0) {
                extracted_links_t* links = ws_extract_links(
                    task->content_buffer->buf, 
                    task->content_buffer->len,
                    content_type,
                    task->url 
                );
                if (links) {
                    // ws_log_info("Extracted %zu links from %s (Content-Type: %s)", links->count, task->url, content_type);
                    // Process the extracted links (e.g., add to queue, filter, normalize)
                    ws_crawler_process_curl_extracted_links(crawler, task->url, links);
                    // Free the extracted links data after processing
                    ws_free_extracted_links(links); 
                } else {
                    ws_log_warn("Failed to extract links from %s or no links found.", task->url);
                }
            } else {
                ws_log_debug("No content to extract links from for URL: %s", task->url);
            }
        } else {
            ws_log_warn("URL %s returned HTTP error code: %ld", task->url, http_code);
            if (crawler->error_callback) {
                crawler->error_callback(crawler, task->url, curl_code, crawler->user_data);
            }
        }
    } else {
        ws_log_error("Failed to crawl URL: %s (Curl error: %d - %s)", 
                    task->url, curl_code, curl_easy_strerror(curl_code));
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

        // Note: ws_http_get takes ownership of task. It will be freed in ws_crawl_http_complete_cb
        if (!ws_http_get(crawler->http_client, task->url,
                         ws_crawl_http_header_cb,
                         ws_crawl_http_data_cb,
                         ws_crawl_http_complete_cb, task)) {
            ws_log_error("Failed to start HTTP GET for URL: %s", task->url);
            ws_crawl_task_free(task); 
            continue;
        }
        
        crawler->active_requests++;
        // ws_log_debug("Active requests: %d, URLs in queue: %zu", crawler->active_requests, crawler->url_queue_size);
    }

    // If no more active requests and no more URLs in queue, crawling might be done.
    if (crawler->active_requests == 0 && crawler->url_queue_size == 0) {
        ws_log_info("Crawler finished all pending tasks.");
        ws_event_loop_stop(crawler->event_loop); // Stop the event loop if desired
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

    if (ws_extract_init() != 0) {
        ws_log_error("Failed to initialize ws_extract module for crawler.");
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
    // ws_log_debug("%s (Queue size: %zu)", url, crawler->url_queue_size);

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

    ws_extract_cleanup();

    zfree(crawler);
    ws_log_info("Crawler freed successfully.");
}