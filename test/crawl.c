#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
// #include <unistd.h> // Not needed if getenv is not used

// Your library headers
#include <core/ws_event.h>
#include <core/ws_log.h>
#include <ws_malloc.h>
#include <ws_util.h>
#include <ws_http.h>
#include <ws_cookie.h>

// --- Crawler Configuration ---
#define MAX_REDIRECTS 5 // Max number of redirects to follow
#define REQUEST_TIMEOUT_MS 15000 // Request timeout in milliseconds

// --- Global Event Base and Active Request Count ---
static ws_event_base *g_event_base = NULL;
static volatile int active_requests = 0; // Tracks the number of active requests

// --- Crawler Context Structure ---
typedef struct crawler_context {
    char *url;          // The URL currently being crawled
    int redirect_count; // Number of redirects followed for this request chain
} crawler_context;

// --- Helper Function: Frees Crawler Context ---
static void free_crawler_context(crawler_context *ctx) {
    if (ctx) {
        zfree(ctx->url);
        zfree(ctx);
    }
}

// --- Forward Declaration for Request Initiation ---
static void start_crawl_request(ws_event_base *we, const char *url_to_crawl, int redirect_count);

// --- HTTP Response Handler Callback ---
void crawl_response_handler(int status_code, struct evkeyvalq *headers,
                           const char *body_data, size_t body_len,
                           void *user_data, int error_code) {
    crawler_context *ctx = (crawler_context *)user_data;
    ws_event_base *we = g_event_base; // Use the global event base

    ws_log_info("\n--- Crawl Response for URL: %s (Redirects: %d) ---", ctx->url, ctx->redirect_count);
    ws_log_info("Status Code: %d", status_code);
    ws_log_info("Error Code: %d (0 = success, -1 = timeout, -2 = connection error)", error_code);

    if (error_code == 0 && status_code >= 200 && status_code < 300) {
        ws_log_info("Content Length: %zu bytes", body_len);
        if (body_data && body_len > 0) {
            // Print a preview of the response body (e.g., first 500 bytes)
            printf("Body preview:\n%.*s...\n", (int) (body_len > 500 ? 500 : body_len), body_data);
        } else {
            ws_log_info("[No body content or empty]");
        }
        ws_log_info("Crawl SUCCESS for: %s", ctx->url);
    } else if (status_code >= 300 && status_code < 400) {
        // Handle redirects
        const char *location = evhttp_find_header(headers, "Location");
        if (location && ctx->redirect_count < MAX_REDIRECTS) {
            ws_log_info("Received redirect (%d) to: %s", status_code, location);
            // Re-initiate the request to the new Location
            start_crawl_request(we, location, ctx->redirect_count + 1);
        } else if (location) {
            ws_log_warn("Max redirects (%d) reached for %s. Not following: %s",
                        MAX_REDIRECTS, ctx->url, location);
        } else {
            ws_log_warn("Redirect (%d) received for %s but no Location header found!", status_code, ctx->url);
        }
    } else {
        ws_log_error("Crawl FAILED for: %s. Status %d, Error %d.", ctx->url, status_code, error_code);
    }

    // Free the context for the current request
    free_crawler_context(ctx);

    // Decrement active request count, and stop the event loop if all requests are done
    active_requests--;
    ws_log_info("Active requests remaining: %d", active_requests);
    if (active_requests == 0) {
        ws_log_info("All crawl requests completed. Stopping event loop.");
        ws_event_stop(we);
    }
    ws_log_info("--------------------------------------------------\n");
}

// --- Initiates a Crawl Request ---
static void start_crawl_request(ws_event_base *we, const char *url_to_crawl, int redirect_count) {
    crawler_context *ctx = zcalloc(sizeof(crawler_context));
    if (!ctx) {
        ws_log_error("Failed to allocate crawler context for %s.", url_to_crawl);
        active_requests--; // Decrease count even on allocation failure
        if (active_requests == 0) ws_event_stop(we);
        return;
    }
    ctx->url = strdup(url_to_crawl);
    ctx->redirect_count = redirect_count;

    if (!ctx->url) {
        ws_log_error("Failed to duplicate URL for crawler context: %s.", url_to_crawl);
        free_crawler_context(ctx);
        active_requests--;
        if (active_requests == 0) ws_event_stop(we);
        return;
    }

    ws_http_request_options options = {0};
    options.url = ctx->url;
    options.method = WS_HTTP_GET;
    options.timeout_ms = REQUEST_TIMEOUT_MS;
    options.headers = NULL; // No custom headers for now
    options.body_data = NULL;
    options.body_len = 0;
    // Proxy options are intentionally left as NULL/0, as we're not using proxies

    ws_log_info("Starting crawl request for: %s (Redirect count: %d)", options.url, redirect_count);
    ws_event_handle *handle = ws_event_http_request(we, &options, crawl_response_handler, ctx);
    if (!handle) {
        ws_log_error("Failed to send crawl request for URL: %s.", options.url);
        free_crawler_context(ctx);
        active_requests--;
        if (active_requests == 0) ws_event_stop(we);
    } else {
        active_requests++;
        ws_log_info("Request sent for ID: %lld. Total active: %d", handle->id, active_requests);
    }
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <URL_to_crawl>\n", argv[0]);
        fprintf(stderr, "Example: %s https://www.example.com\n", argv[0]);
        return 1;
    }

    // Initialize the event base
    g_event_base = ws_event_new();
    if (!g_event_base) {
        fprintf(stderr, "Could not initialize ws_event_base!\n");
        return 1;
    }
    ws_log_info("ws_event_base initialized.");
    ws_log_info("Assuming SSL context initialized internally by ws_event_new for HTTPS support.");

    // Start the first crawl request
    start_crawl_request(g_event_base, argv[1], 0);

    // Run the event loop until all requests are completed or stopped
    ws_log_info("Starting event loop...");
    ws_event_loop(g_event_base);

    ws_log_info("Event loop stopped. Final active requests: %d", active_requests);

    // Clean up resources
    ws_event_free(g_event_base);
    ws_log_info("ws_event_base freed. Program finished.");

    return 0;
}