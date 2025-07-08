#include <stdio.h>
#include <string.h>
#include <unistd.h> // For sleep
#include <ws_event.h> // Your event loop
#include <ws_log.h>   // Your logging
#include <ws_crawl.h> // Your crawler

// Global flag to indicate if crawling should stop (for graceful shutdown)
static bool g_crawl_finished = false;

// Callback for successfully crawled pages
void my_page_callback(ws_crawler_t *crawler, const char *url, long http_code,
                      const char *content, size_t content_len, void *user_data) {
    (void)crawler; // Unused parameter
    (void)user_data; // Unused parameter
    ws_log_info("Page crawled successfully: %s (HTTP %ld)", url, http_code);
    // ws_log_debug("Content snippet (first 100 bytes): %.*s%s",
    //              (int) (content_len > 100 ? 100 : content_len), content,
    //              content_len > 100 ? "..." : "");

    // Example: Stop after crawling a specific page for testing
    // if (strstr(url, "example.com/some_specific_page")) {
    //     ws_log_info("Stopping crawler after specific page.");
    //     g_crawl_finished = true;
    //     ws_event_break_loop(ws_crawler_get_event_loop(crawler)); // Assuming a getter for event loop
    // }
}

// Callback for crawling errors
void my_error_callback(ws_crawler_t *crawler, const char *url, CURLcode curl_code, void *user_data) {
    (void)crawler; // Unused parameter
    (void)user_data; // Unused parameter
    ws_log_error("Error crawling URL: %s (Curl error: %d - %s)",
                 url ? url : "N/A", (int)curl_code, curl_easy_strerror(curl_code));
}

// Function to get event loop from crawler (not in ws_crawl.h yet, assume it exists for this example)
// In a real scenario, you'd either pass the event loop around or make a getter.
// For now, let's assume ws_crawler_new returns it directly, or we store it globally for simplicity.
// For this example, we'll store event_loop globally.
ws_event_loop_t *global_event_loop_for_main = NULL;

// Example function to stop the event loop after some time or condition
void stop_crawler_timer_cb(void *user_data) {
    (void)user_data;
    ws_log_info("Stop timer fired. Breaking event loop.");
    g_crawl_finished = true;
    if (global_event_loop_for_main) {
        ws_event_loop_stop(global_event_loop_for_main);
    }
}


int main() {
    // 2. Create the event loop
    ws_event_loop_t *event_loop = ws_event_loop_new();
    if (!event_loop) {
        ws_log_error("Failed to create event loop.");
        return 1;
    }
    global_event_loop_for_main = event_loop; // Store globally for stop timer

    // 3. Create the crawler instance
    int max_concurrent_requests = 10; // Example: Allow 5 concurrent requests
    ws_crawler_t *crawler = ws_crawler_new(event_loop, max_concurrent_requests,
                                           my_page_callback, my_error_callback,
                                           NULL); // No user_data for crawler for this example
    if (!crawler) {
        ws_log_error("Failed to create crawler.");
        ws_event_loop_free(event_loop);
        return 1;
    }

    // 4. Add some initial URLs to crawl
    // It's important to choose URLs that have 'href' links for the simple link extraction to work.
    // Replace with real URLs you want to crawl for testing.
    // ws_crawler_add_url(crawler, "https://www.jd.com/");
    // ws_crawler_add_url(crawler, "http://www.google.com/"); // Google might block quickly
    ws_crawler_add_url(crawler, "http://www.baidu.com/");   // Baidu might block quickly
    ws_crawler_add_url(crawler, "http://ws.cc/"); // Dummy URL, likely won't resolve

    // Add a timer to stop the crawler after some seconds for testing purposes
    // In a real application, you'd stop based on queue empty or other criteria.
    ws_event_t *stop_timer = ws_event_new_timer(event_loop, 600000, false, stop_crawler_timer_cb, NULL); // Stop after 30 seconds
    if (!stop_timer || !ws_event_add(stop_timer)) {
        ws_log_error("Failed to set up stop timer.");
        // Continue anyway, but crawler won't auto-stop
    }


    // 5. Start the crawling process (this will kick off the dispatching)
    ws_crawler_start(crawler);

    // 6. Run the event loop (this blocks until ws_event_break_loop is called)
    ws_log_info("Starting event loop...");
    ws_event_loop_dispatch(event_loop);
    ws_log_info("Event loop stopped.");

    // 7. Clean up resources
    ws_crawler_free(crawler);
    if (stop_timer) {
        ws_event_del(stop_timer);
        ws_event_free(stop_timer);
    }
    ws_event_loop_free(event_loop);

    ws_log_info("Application finished.");
    return 0;
}