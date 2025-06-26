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
#include <sys/queue.h> // For TAILQ_FOREACH
#include <event2/event.h>
#include <event2/http.h>
#include <event2/util.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h> // For struct evkeyvalq
#include <event2/bufferevent_ssl.h>
#include <evhttp.h>
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_http.h>
#include <ws_ssl.h>
#include <ws_util.h>

// Internal context for HTTP requests
typedef struct ws_http_internal_context {
    ws_event_handle *handle;            // 指向外部 ws_event_handle
    struct evhttp_request *req;         // libevent HTTP请求对象
    struct evhttp_connection *conn;     // libevent HTTP连接对象
    struct event *timeout_ev;           // 超时事件
    ws_event_http_cb callback;          // 用户HTTP回调
    void *user_data;                    // 用户参数
    struct evbuffer *response_buffer;   // 响应体缓冲
    SSL *ssl_obj;                       // HTTPS连接的SSL对象 (now specifically the SSL* created per connection)
} ws_http_internal_context;

// Internal function: Cleans up the HTTP request context
static void cleanup_http_context(ws_http_internal_context *ctx) {
    if (!ctx) return;

    if (ctx->timeout_ev) {
        event_del(ctx->timeout_ev);
        event_free(ctx->timeout_ev);
        ctx->timeout_ev = NULL;
    }
    if (ctx->response_buffer) {
        evbuffer_free(ctx->response_buffer);
        ctx->response_buffer = NULL;
    }
    // req and conn are implicitly freed by libevent upon request completion/error,
    // or by evhttp_connection_free/evhttp_request_free in error paths where they are not yet owned by libevent.
    // The SSL* object (ctx->ssl_obj) is managed by the bufferevent (BEV_OPT_CLOSE_ON_FREE)
    // which in turn is managed by the evhttp_connection. No manual SSL_free here.
    zfree(ctx);
}

// HTTP request timeout callback
static void http_request_timeout_cb(evutil_socket_t fd, short what, void *arg) {
    UNUSED(fd);
    UNUSED(what);

    ws_http_internal_context *ctx = (ws_http_internal_context *)arg;
    if (!ctx) return;

    ws_log_error("HTTP Request ID %lld timed out for handle %p.",
                  ctx->handle->id, (void*)ctx->handle);

    // Increment failed requests count
    ctx->handle->base->failed_requests++;

    // Call user callback and pass timeout error
    if (ctx->callback) {
        // -1 indicates a timeout error
        ctx->callback(0, NULL, NULL, 0, ctx->user_data, -1);
    }

    // Ensure this handle is removed from ws_event_base's red-black tree
    if (ctx->handle) {
        ws_event_del(ctx->handle); // This will trigger ws_rb_item_func to free ws_event_handle
    }
    // Free internal context
    cleanup_http_context(ctx);
}

/* HTTP request completion callback (called by libevent) */
static void ws_event_internal_http_cb(struct evhttp_request *req, void *arg) {
    ws_http_internal_context *ctx = (ws_http_internal_context *)arg;
    if (!ctx) return;

    // Request completed, cancel the timeout event
    if (ctx->timeout_ev) {
        event_del(ctx->timeout_ev);
        event_free(ctx->timeout_ev);
        ctx->timeout_ev = NULL; // Prevent double freeing
    }

    int status_code = 0;
    const char *body_data = NULL;
    size_t body_len = 0;
    int error_code = 0; // 0 indicates success

    if (req) {
        status_code = evhttp_request_get_response_code(req);
        struct evbuffer *buf = evhttp_request_get_input_buffer(req);
        if (buf) {
            body_len = evbuffer_get_length(buf);
            body_data = (const char *)evbuffer_pullup(buf, body_len);
        }

        /* HTTP error code or no response code (may indicate connection issue)
         * -2 for connection/protocol error, others are HTTP status codes */
        if (status_code >= 400 || status_code == 0) {
            error_code = status_code == 0 ? -2 : status_code;
            ctx->handle->base->failed_requests++;
            ws_log_error("HTTP request ID %lld to %s failed with status %d, error code %d",
                         ctx->handle->id, evhttp_request_get_uri(req), status_code, error_code);
        } else {
            ctx->handle->base->success_requests++;
            ws_log_info("HTTP request ID %lld to %s completed with status %d.",
                         ctx->handle->id, evhttp_request_get_uri(req), status_code);
        }
    } else {
        // Request failed, e.g., connection error, DNS resolution failure
        ws_log_error("HTTP request ID %lld failed (no response or connection error).", ctx->handle->id);
        error_code = -2; // e.g., -2 indicates connection or protocol error
        ctx->handle->base->failed_requests++;
    }

    ctx->handle->base->total_requests++;

    // Call user callback
    if (ctx->callback) {
        ctx->callback(status_code, req ? evhttp_request_get_input_headers(req) : NULL,
                      body_data, body_len, ctx->user_data, error_code);
    }

    // Ensure this handle is removed from ws_event_base's red-black tree
    // ws_event_del will trigger ws_rb_item_func to free ws_event_handle
    if (ctx->handle) {
        ws_event_del(ctx->handle);
        ctx->handle = NULL; // Prevent double free
    }
    // Free internal context
    cleanup_http_context(ctx);
}

/**
 * @brief Sets up headers for an HTTP request.
 * @param req The evhttp_request object.
 * @param options The HTTP request options containing custom headers.
 * @param host The host name for the "Host" header.
 */
static void setup_http_request_headers(struct evhttp_request *req,
                                       const ws_http_request_options *options,
                                       const char *host) {
    // Get the output headers map for the request
    struct evkeyvalq *output_headers = evhttp_request_get_output_headers(req);

    // Add custom headers from options
    if (options->headers) {
        struct evkeyval *header;
        TAILQ_FOREACH(header, options->headers, next) {
            evhttp_add_header(output_headers, header->key, header->value);
        }
    }

    // Add default headers if not already present
    // "Host" header
    if (!evhttp_find_header(output_headers, "Host")) {
        evhttp_add_header(output_headers, "Host", host);
    }
    // "Connection: close" header (simplified, no Keep-Alive)
    if (!evhttp_find_header(output_headers, "Connection")) {
        evhttp_add_header(output_headers, "Connection", "close");
    }

    // Add Content-Type for POST/PUT if body exists and header is not provided
    if ((options->method == WS_HTTP_POST || options->method == WS_HTTP_PUT)
        && options->body_data && options->body_len > 0) {
        if (!evhttp_find_header(output_headers, "Content-Type")) {
            evhttp_add_header(output_headers, "Content-Type", "application/x-www-form-urlencoded");
        }
    }
}

/**
 * @brief Sends an HTTP request to the event loop.
 */
ws_event_handle *ws_event_http_request(ws_event_base *we,
    const ws_http_request_options *options,
    ws_event_http_cb callback, void *arg) {

    struct evhttp_connection *conn = NULL;
    struct evhttp_request *req = NULL;
    ws_event_handle *h = NULL;
    ws_http_internal_context *internal_ctx = NULL;
    struct evhttp_uri *uri = NULL;
    SSL *ssl_obj = NULL;

    if (!we || !we->base || !options || !options->url || !callback) {
        ws_log_error("Invalid parameters for ws_event_http_request.");
        return NULL;
    }

    uri = evhttp_uri_parse(options->url);
    if (!uri) {
        ws_log_error("Failed to parse URL: %s", options->url);
        return NULL;
    }

    const char *host = evhttp_uri_get_host(uri);
    int port = evhttp_uri_get_port(uri);
    const char *path = evhttp_uri_get_path(uri);
    const char *scheme = evhttp_uri_get_scheme(uri);
    const char *query = evhttp_uri_get_query(uri);

    if (!host) {
        ws_log_error("URL has no host: %s", options->url);
        goto err_uri;
    }

    // Handle default port
    if (port == -1) {
        port = (scheme && strcasecmp(scheme, "https") == 0) ? 443 : 80;
    }

    // Build the full path (including query string)
    char full_path[2048];
    if (!path || path[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "/%s", query ? query : "");
    } else if (query && query[0] != '\0') {
        snprintf(full_path, sizeof(full_path), "%s?%s", path, query);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", path);
    }
    full_path[sizeof(full_path) - 1] = '\0';

    h = zcalloc(sizeof(ws_event_handle));
    if (!h) {
        ws_log_error("Failed to allocate memory for ws_event_handle (HTTP event).");
        goto err_uri;
    }

    internal_ctx = zcalloc(sizeof(ws_http_internal_context));
    if (!internal_ctx) {
        ws_log_error("Failed to allocate memory for internal HTTP context.");
        goto err_handle;
    }
    // Initialize internal_ctx->ssl_obj to NULL to prevent attempts to free
    // an uncreated SSL object in error paths.
    internal_ctx->ssl_obj = NULL; 

    h->id = we->next_event_id++;
    h->type = WS_EVENT_HTTP;
    h->base = we;
    h->http.callback = callback;
    h->arg = arg;
    h->http.internal_ctx = internal_ctx;

    internal_ctx->handle = h;
    internal_ctx->callback = callback;
    internal_ctx->user_data = arg;
    internal_ctx->response_buffer = evbuffer_new();
    if (!internal_ctx->response_buffer) {
        ws_log_error("Failed to create response buffer for HTTP request ID %lld.", h->id);
        goto err_internal_ctx;
    }

    // **HTTPS connection handling**
    if (scheme && strcmp(scheme, "https") == 0) {
        if (!we->ssl_ctx) {
            ws_log_error("SSL_CTX is not initialized for HTTPS request to %s. Aborting.", options->url);
            goto err_response_buffer;
        }
        ssl_obj = ws_ssl_new_connection_ssl(we->ssl_ctx, host);
        if (!ssl_obj) {
            ws_log_error("Failed to create SSL object for HTTPS connection to %s:%d.", host, port);
            goto err_response_buffer;
        }
        internal_ctx->ssl_obj = ssl_obj; // Store SSL object for potential freeing if connection creation fails

        /* Create bufferevent for SSL communication
         * -1 for socket means bufferevent will create one
         * BUFFEREVENT_SSL_CONNECTING for client mode
         * BEV_OPT_CLOSE_ON_FREE ensures SSL object and socket are freed when bufferevent is freed.
         * BEV_OPT_DEFER_CALLBACKS is generally good for performance.*/
        struct bufferevent *bev = bufferevent_openssl_socket_new(
            we->base, -1, ssl_obj, 
            BUFFEREVENT_SSL_CONNECTING, 
            BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS
        );
        if (!bev) {
            ws_log_error("Failed to create bufferevent for HTTPS connection.");
            SSL_free(ssl_obj);
            goto err_response_buffer;
        }

        /* Create evhttp_connection from the bufferevent
         * dns_base can be NULL because bufferevent_socket_connect_hostname will handle DNS */
        conn = evhttp_connection_base_bufferevent_new(we->base, NULL, bev, host, port);
        if (!conn) {
            ws_log_error("Failed to create HTTPS evhttp_connection.");
            bufferevent_free(bev);
            goto err_response_buffer;
        }
    } else {
        // HTTP connection
        conn = evhttp_connection_base_new(we->base, NULL, host, port);
    }

    if (!conn) {
        ws_log_error("Failed to create HTTP(S) connection for %s:%d.", host, port);
        // This is where a goto directly leads to err_ssl_obj_or_response_buffer.
        // It correctly handles freeing ssl_obj if it was created but not taken ownership of by libevent.
        goto err_ssl_obj_or_response_buffer; 
    }
    internal_ctx->conn = conn;

    req = evhttp_request_new(ws_event_internal_http_cb, internal_ctx);
    if (!req) {
        ws_log_error("Failed to create HTTP request for URL: %s.", options->url);
        goto err_conn;
    }
    internal_ctx->req = req;

    // Set request timeout (libevent expects seconds)
    if (options->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = options->timeout_ms / 1000;
        tv.tv_usec = (options->timeout_ms % 1000) * 1000;
        internal_ctx->timeout_ev = event_new(we->base, -1, EV_TIMEOUT,
                                             http_request_timeout_cb,
                                             internal_ctx);
        if (internal_ctx->timeout_ev) {
            event_add(internal_ctx->timeout_ev, &tv);
        } else {
            ws_log_error("Warning: Could not create timeout event for HTTP request ID %lld.", h->id);
        }
    }

    setup_http_request_headers(req, options, host);

    // Handle POST/PUT data
    if ((options->method == WS_HTTP_POST || options->method == WS_HTTP_PUT)
        && options->body_data && options->body_len > 0) {
        struct evbuffer *output_buffer = evhttp_request_get_output_buffer(req);
        evbuffer_add(output_buffer, options->body_data, options->body_len);
    }

    /* Set HTTP method */
    int ev_http_method;
    switch (options->method) {
    case WS_HTTP_GET:    ev_http_method = EVHTTP_REQ_GET;    break;
    case WS_HTTP_POST:   ev_http_method = EVHTTP_REQ_POST;   break;
    case WS_HTTP_PUT:    ev_http_method = EVHTTP_REQ_PUT;    break;
    case WS_HTTP_DELETE: ev_http_method = EVHTTP_REQ_DELETE; break;
    default:
        ws_log_error("Unsupported HTTP method: %d for URL %s.", options->method, options->url);
        goto err_req;
    }

    /* Send request */
    if (evhttp_make_request(conn, req, ev_http_method, full_path) == -1) {
        ws_log_error("Failed to make HTTP request to %s.", options->url);
        goto err_req;
    }

    /* Insert the event handle into the red-black tree */
    if (ws_insert_event(we, h) == NULL) {
        ws_log_error("Failed to insert HTTP event handle %lld into red-black tree. Request might still complete.", h->id);
        // NOTE: The request may have been sent, but the handle is not managed by the tree.
        // We cannot simply clean up req and conn at this point, as they are taken over by libevent.
        // The lifecycle of h will be managed by _http_request_done or http_request_timeout_cb.
        // Returning NULL here means that the "start" failed (but the network request may have been sent).
        evhttp_uri_free(uri);
        return NULL;
    }

    evhttp_uri_free(uri);
    return h;

err_req:
    if (req) evhttp_request_free(req); // Only if failed before evhttp_make_request took ownership
err_conn:
    if (conn) evhttp_connection_free(conn); // Only if failed before conn was associated with req
err_ssl_obj_or_response_buffer: 
    if (internal_ctx && internal_ctx->ssl_obj) {
        // If ssl_obj was created but not yet taken ownership by libevent, free it here.
        SSL_free(internal_ctx->ssl_obj);
    }
err_response_buffer:
    if (internal_ctx && internal_ctx->response_buffer)
        evbuffer_free(internal_ctx->response_buffer);
err_internal_ctx:
    if (internal_ctx) zfree(internal_ctx);
err_handle:
    if (h) zfree(h);
err_uri:
    if (uri) evhttp_uri_free(uri);
    return NULL;
}
