// main.c (Example Usage)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h> // For sleep
#include <ws_log.h>
#include <ws_event.h>
#include <ws_http.h>

// Global event loop and HTTP client pointers for signal handler and cleanup
ws_event_loop_t *g_main_loop = NULL;
ws_http_client_t *g_http_client = NULL;

// Store received data (for simplicity, in a real app use dynamic buffer)
char g_response_buffer[4096];
size_t g_response_len = 0;

// --- HTTP Callbacks ---

void my_http_header_callback(const char *header, void *user_data) {
    ws_log_debug("Header (Context: %s): %s", (char*)user_data, header);
}

void my_http_data_callback(const char *data, size_t size, void *user_data) {
    // For demonstration, accumulate data. In real apps, handle streaming.
    if (g_response_len + size < sizeof(g_response_buffer)) {
        memcpy(g_response_buffer + g_response_len, data, size);
        g_response_len += size;
    } else {
        ws_log_warn("Response buffer full, discarding data.");
    }
}

void my_http_complete_callback(ws_http_request_t *request, long http_code, CURLcode curl_code, void *user_data) {
    const char *context = (const char *)user_data;
    ws_log_info("HTTP request (Context: %s) completed.", context);
    ws_log_info("  HTTP Status: %ld", http_code);
    ws_log_info("  Curl Result: %d (%s)", (int)curl_code, curl_easy_strerror(curl_code));

    if (curl_code == CURLE_OK) {
        // Null-terminate the accumulated response data for printing
        g_response_buffer[g_response_len] = '\0';
        ws_log_info("  Response Body (%zu bytes):\n%s", g_response_len, g_response_buffer);
    } else {
        ws_log_error("  HTTP request failed.");
    }

    // Reset buffer for next request
    g_response_len = 0;
    memset(g_response_buffer, 0, sizeof(g_response_buffer));
}

// --- Global Timer Callback (for general app logic, not curl internal) ---
void app_timer_callback(void *user_data) {
    static int count = 0;
    ws_log_info("Application timer fired! Count: %d", ++count);
    if (count == 3) {
        ws_log_info("Time to stop the loop!");
        ws_event_loop_stop(g_main_loop);
    }
}

// --- Signal Handler ---
void signal_handler(int sig) {
    if (sig == SIGINT) {
        ws_log_info("SIGINT received. Shutting down event loop...");
        if (g_main_loop) {
            ws_event_loop_stop(g_main_loop);
        } else {
            exit(EXIT_SUCCESS);
        }
    }
}

int main() {
    // 0. Set up logging (optional, but good for debugging)
    // ws_log_set_level(WS_LOG_INFO); // Set to WS_LOG_DEBUG for more verbosity

    // Set up signal handler
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        ws_log_error("Failed to set SIGINT handler.");
        return EXIT_FAILURE;
    }

    // 1. Create an event loop
    g_main_loop = ws_event_loop_new();
    if (!g_main_loop) {
        return EXIT_FAILURE;
    }

    // 2. Create an HTTP client, linking it to our event loop
    g_http_client = ws_http_client_new(g_main_loop);
    if (!g_http_client) {
        ws_event_loop_free(g_main_loop);
        return EXIT_FAILURE;
    }

    // 3. Create an application timer (independent of curl's internal timers)
    ws_event_t *app_timer = ws_event_new_timer(g_main_loop, 1000, true, app_timer_callback, "App Timer Context");
    if (!app_timer || !ws_event_add(app_timer)) {
        ws_log_error("Failed to add application timer.");
    }

    // 4. Make an asynchronous HTTP GET request
    // IMPORTANT: Use URLs that are accessible. For testing, 'http://example.com' or a local server.
    // Be aware of firewalls/proxies.
    ws_log_info("Making GET request to http://example.com");
    ws_http_get(g_http_client, "http://example.com",
                my_http_header_callback,
                my_http_data_callback,
                my_http_complete_callback,
                "GET Request Context");

    // 5. Make an asynchronous HTTP POST request
    ws_log_info("Making POST request to http://httpbin.org/post");
    const char *post_data = "key1=value1&key2=value2";
    ws_http_post(g_http_client, "http://httpbin.org/post",
                 post_data, strlen(post_data),
                 my_http_header_callback,
                 my_http_data_callback,
                 my_http_complete_callback,
                 "POST Request Context");

    // 6. Dispatch the event loop
    ws_log_info("Starting main event loop dispatch...");
    ws_event_loop_dispatch(g_main_loop);

    // 7. Cleanup
    ws_log_info("Main loop stopped. Cleaning up resources...");
    ws_http_client_free(g_http_client); // This will also free any pending requests
    ws_event_free(app_timer);           // Free app timer
    ws_event_loop_free(g_main_loop);

    // Call curl_global_cleanup only once at the very end of your application's lifecycle.
    // If you have multiple ws_http_client_t instances, ensure this is truly the last cleanup.
    curl_global_cleanup(); // Important for libcurl

    ws_log_info("Application exited cleanly.");
    return EXIT_SUCCESS;
}