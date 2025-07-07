#ifndef __WS_CRAWL_H__
#define __WS_CRAWL_H__

#include <stddef.h> // For size_t
#include <stdbool.h> // For bool
#include <ws_http.h> // Include your ws_http module

// Forward declaration
typedef struct ws_crawler ws_crawler_t;

/**
 * @brief Callback function type for when a page has been successfully crawled.
 *
 * @param crawler The crawler instance.
 * @param url The URL that was crawled.
 * @param http_code The HTTP status code of the response.
 * @param content The received page content (body). This buffer is managed by the crawler,
 * do not free it. It will be valid only within this callback.
 * @param content_len Length of the content.
 * @param user_data User-provided data passed during crawler creation.
 */
typedef void (*ws_crawl_page_cb_fn)(ws_crawler_t *crawler, const char *url,
                                    long http_code, const char *content,
                                    size_t content_len, void *user_data);

/**
 * @brief Callback function type for when an error occurs during crawling.
 *
 * @param crawler The crawler instance.
 * @param url The URL for which the error occurred (can be NULL if error is global).
 * @param curl_code The CURLcode indicating the error.
 * @param user_data User-provided data passed during crawler creation.
 */
typedef void (*ws_crawl_error_cb_fn)(ws_crawler_t *crawler, const char *url,
                                     CURLcode curl_code, void *user_data);

/**
 * @brief Creates a new web crawler instance.
 *
 * @param event_loop The ws_event_loop_t to use for asynchronous operations.
 * @param max_concurrent_requests Maximum number of concurrent HTTP requests.
 * @param page_callback Callback to be invoked when a page is successfully crawled.
 * @param error_callback Callback to be invoked when a crawling error occurs.
 * @param user_data User-defined data to be passed to all callbacks.
 * @return A pointer to the new ws_crawler_t instance, or NULL on failure.
 */
ws_crawler_t *ws_crawler_new(ws_event_loop_t *event_loop,
                             int max_concurrent_requests,
                             ws_crawl_page_cb_fn page_callback,
                             ws_crawl_error_cb_fn error_callback,
                             void *user_data);

/**
 * @brief Adds a URL to the crawling queue.
 *
 * @param crawler The crawler instance.
 * @param url The URL to add.
 * @return true if the URL was successfully added, false otherwise (e.g., duplicate URL or out of memory).
 */
bool ws_crawler_add_url(ws_crawler_t *crawler, const char *url);

/**
 * @brief Starts the crawling process.
 * Note: The event loop must be running for the crawler to perform requests.
 *
 * @param crawler The crawler instance.
 */
void ws_crawler_start(ws_crawler_t *crawler);

/**
 * @brief Stops the crawling process and cleans up resources.
 * Any pending requests will be cancelled.
 *
 * @param crawler The crawler instance.
 */
void ws_crawler_free(ws_crawler_t *crawler);

#endif // WS_CRAWL_H