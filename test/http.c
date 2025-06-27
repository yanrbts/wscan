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
 * SUBSTITUTE GOODS OR (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
// test_http.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/queue.h> // For TAILQ_FOREACH
#include <event2/buffer.h>
#include <event2/http.h>
#include <errno.h> // For strerror
#include <stdbool.h> // For bool

// Include your library headers
#include <core/ws_event.h>
#include <core/ws_log.h>
#include <ws_malloc.h>
#include <ws_util.h>
#include <ws_http.h>
// Removed: #include <ws_ssl.h> // As ws_ssl_init_ctx is not external
#include <ws_cookie.h> // For cookie specific functions

// --- Test Macros ---
#define TEST_HTTPBIN_URL            "https://www.163.com"
#define TEST_FLASK_URL_SET_COOKIE   "https://www.jd.com"  // Assuming your Flask server listens on HTTPS
#define TEST_FLASK_URL_CHECK_COOKIE "https://www.baidu.com/"
#define TEST_TIMEOUT_MS             100000 // 10 second timeout

// --- Global Variables for Test Synchronization and Result Passing ---
typedef enum {
    TEST_STEP_IDLE,
    TEST_STEP_HTTPBIN,
    TEST_STEP_FLASK_SET_COOKIE,
    TEST_STEP_FLASK_CHECK_COOKIE,
    TEST_STEP_COMPLETE,
    TEST_STEP_FAILED
} TestStep;

static TestStep current_test_step = TEST_STEP_IDLE;
static ws_event_base *g_event_base = NULL; // Global event base pointer

// --- Helper Function: Print all Set-Cookie Headers ---
void print_set_cookie_headers(struct evkeyvalq *headers) {
    printf("--- Set-Cookie Headers Received ---\n");
    struct evkeyval *header;
    TAILQ_FOREACH(header, headers, next) {
        if (strcasecmp(header->key, "Set-Cookie") == 0) {
            printf("Set-Cookie: %s\n", header->value);
        }
    }
    printf("----------------------------------\n");
}

// HTTP Response Handling Callback Function
void my_http_response_handler(int status_code, struct evkeyvalq *headers,
                              const char *body_data, size_t body_len,
                              void *user_data, int error_code) {
    ws_event_base *we = (ws_event_base *)user_data; // user_data is ws_event_base pointer

    ws_log_info("--- HTTP Response (Step: %d) ---", current_test_step);
    ws_log_info("Status Code: %d", status_code);
    ws_log_info("Error Code: %d (0 = success, -1 = timeout, -2 = connection error)", error_code);

    if (error_code == 0 && status_code >= 200 && status_code < 300) {
        ws_log_info("Headers:");
        if (headers) {
            struct evkeyval *header;
            TAILQ_FOREACH(header, headers, next) {
                ws_log_info("  %s: %s", header->key, header->value);
            }
        }

        ws_log_info("Body (length %zu):", body_len);
        if (body_data) {
            // if (body_len > 500) { // Avoid excessive output
            //     printf("%.*s...\n", 50, body_data);
            // } else {
            //     printf("%.*s\n", (int)body_len, body_data);
            // }
        } else {
            ws_log_info("[No body]");
        }

        // Proceed based on the current test step
        if (current_test_step == TEST_STEP_HTTPBIN) {
            ws_log_info("HTTPS GET to %s successful. Proceeding to Flask Set-Cookie request...", TEST_HTTPBIN_URL);
            current_test_step = TEST_STEP_FLASK_SET_COOKIE;

            ws_http_request_options options = {0};
            options.method = WS_HTTP_GET;
            options.url = TEST_FLASK_URL_SET_COOKIE;
            options.timeout_ms = TEST_TIMEOUT_MS;

            ws_log_info("Sending HTTPS GET request to %s (Set-Cookie).", options.url);
            ws_event_handle *handle = ws_event_http_request(we, &options, my_http_response_handler, we);
            if (!handle) {
                ws_log_error("Failed to send Flask Set-Cookie request.");
                current_test_step = TEST_STEP_FAILED;
                ws_event_stop(we);
            }
        } else if (current_test_step == TEST_STEP_FLASK_SET_COOKIE) {
            ws_log_info("Flask Set-Cookie request to %s successful. Cookies should be in jar.", TEST_FLASK_URL_SET_COOKIE);
            print_set_cookie_headers(headers); // Print Set-Cookie headers

            // At this point, the Cookie Jar should be updated.
            // Now send the second request to /check-cookie to verify cookies are sent.
            current_test_step = TEST_STEP_FLASK_CHECK_COOKIE;

            ws_http_request_options options = {0};
            options.method = WS_HTTP_GET;
            options.url = TEST_FLASK_URL_CHECK_COOKIE;
            options.timeout_ms = TEST_TIMEOUT_MS;

            ws_log_info("Sending HTTPS GET request to %s (Check-Cookie).", options.url);
            ws_event_handle *handle = ws_event_http_request(we, &options, my_http_response_handler, we);
            if (!handle) {
                ws_log_error("Failed to send Flask Check-Cookie request.");
                current_test_step = TEST_STEP_FAILED;
                ws_event_stop(we);
            }
        } else if (current_test_step == TEST_STEP_FLASK_CHECK_COOKIE) {
            ws_log_info("Flask Check-Cookie request to %s successful. Verifying received cookies.", TEST_FLASK_URL_CHECK_COOKIE);
            if (body_data && body_len > 0) {
                // Simple string matching to verify the cookie values returned by Flask
                // Ensure they match the cookie values set by the Flask /set-cookie endpoint
                if (strstr(body_data, "my_session_cookie: abcdef12345") && strstr(body_data, "persistent_cookie: hello_world")) {
                    ws_log_info("Cookie test PASSED: Both session and persistent cookies were sent and received correctly by Flask.");
                } else {
                    ws_log_error("Cookie test FAILED: Cookies were not sent to Flask as expected. Response: %d", (int)body_len);
                }
            } else {
                ws_log_error("Cookie test FAILED: No body received for cookie verification.");
            }
            current_test_step = TEST_STEP_COMPLETE;
            ws_event_stop(we); // All tests complete, stop event loop
        }
    } else {
        ws_log_error("--- HTTP Request Failed! (Step: %d) ---", current_test_step);
        ws_log_error("Error Code: %d", error_code);
        if (error_code == -1) {
            ws_log_error("Reason: Request Timed Out.");
        } else if (error_code == -2) {
            ws_log_error("Reason: Connection/Protocol Error.");
        } else if (status_code >= 400) {
            ws_log_error("Reason: HTTP Status Code Error (HTTP %d).", status_code);
        }
        current_test_step = TEST_STEP_FAILED;
        ws_event_stop(we); // Request failed, stop event loop
    }

    ws_log_info("Requests Stats: Total=%d, Success=%d, Failed=%d",
                we->total_requests, we->success_requests, we->failed_requests);
    ws_log_info("--------------------------------------------------\n");
}

int main() {
    // ws_log_set_level(WS_LOG_DEBUG); // Set log level to DEBUG for more verbose output

    // 1. Initialize the event base.
    // This is where the internal SSL context initialization is expected to happen.
    g_event_base = ws_event_new();
    if (!g_event_base) {
        fprintf(stderr, "Could not initialize ws_event_base!\n");
        return 1;
    }
    ws_log_info("ws_event_base initialized.");

    // No explicit ws_ssl_init_ctx call here, assuming it's handled internally by ws_event_new
    // If your library requires a specific CA cert path for internal SSL init,
    // you would need to adjust ws_event_new or provide a configuration mechanism.
    ws_log_info("Assuming SSL context initialized internally by ws_event_new.");


    // 2. Send the first HTTPS GET request to httpbin.org (to test SSL connection)
    current_test_step = TEST_STEP_HTTPBIN;
    ws_http_request_options httpbin_options = {0};
    httpbin_options.method = WS_HTTP_GET;
    httpbin_options.url = TEST_HTTPBIN_URL;
    httpbin_options.timeout_ms = TEST_TIMEOUT_MS;

    ws_log_info("Sending HTTPS GET request to %s (Test SSL).", httpbin_options.url);
    ws_event_handle *httpbin_req_handle = ws_event_http_request(g_event_base, &httpbin_options, my_http_response_handler, g_event_base);
    if (!httpbin_req_handle) {
        ws_log_error("Failed to send HTTPS GET request to httpbin.org.");
        ws_event_free(g_event_base);
        return 1;
    }

    // 3. Run the event loop
    ws_log_info("Starting event loop...");
    ws_event_loop(g_event_base); // Blocks until all events are complete or ws_event_stop is called

    ws_log_info("Event loop stopped. Final Test Status: %d", current_test_step);

    // 4. Clean up resources
    ws_event_free(g_event_base);
    ws_log_info("ws_event_base freed. Program finished.");

    return (current_test_step == TEST_STEP_COMPLETE) ? 0 : 1;
}