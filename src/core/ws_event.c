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
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_event.h>
#include <ws_util.h>
#include <string.h>
#include <sys/queue.h> // For TAILQ_FOREACH
#include <event2/dns.h> // For DNS resolution in HTTP client

// 内部使用的HTTP请求上下文结构体
typedef struct ws_http_internal_context {
    ws_event_handle *handle;           // 指向外部 ws_event_handle
    struct evhttp_request *req;        // libevent HTTP请求对象
    struct evhttp_connection *conn;    // libevent HTTP连接对象 (可能被重用或由libevent管理)
    struct event *timeout_ev;          // 超时事件
    ws_event_http_cb callback;         // 用户提供的HTTP回调
    void *user_data;                   // 用户传递的参数
    struct evbuffer *response_buffer;  // 用于积累响应体数据
    SSL *ssl_obj;                      // **新增: HTTPS 连接的 SSL 对象，在错误路径需要释放**
} ws_http_internal_context;

static int ws_rb_comparison_func(const void *rb_a, const void *rb_b, void *rb_param) {
    UNUSED(rb_param);
    const ws_event_handle *a = (const ws_event_handle *)rb_a;
    const ws_event_handle *b = (const ws_event_handle *)rb_b;

    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0; // IDs are unique, this should theoretically not happen
}

// Red-black tree item free function: frees only the ws_event_handle itself and its libevent event object
static void ws_rb_item_func(void *rb_item, void *rb_param) {
    UNUSED(rb_param);

    if (rb_item) {
        ws_event_handle *h = (ws_event_handle *)rb_item;

        switch (h->type) {
            case WS_EVENT_NORMAL:
                if (h->normal.ev) {
                    event_del(h->normal.ev);
                    event_free(h->normal.ev);
                }
                break;
            case WS_EVENT_HTTP:
                // The internal HTTP context (ws_http_internal_context) is freed in cleanup_http_context or on timeout.
                // Ensure h->http.internal_ctx has already been freed via cleanup_http_context at this point.
                // Only free the ws_event_handle itself here.
                break;
            case WS_EVENT_TIME:
                if (h->time.ev) {
                    event_del(h->time.ev);
                    event_free(h->time.ev);
                }
                break;
            default:
                ws_log_error("Unknown event type for item_func: %d", h->type);
                break;
        }
        // Free the event handle itself
        zfree(h);
    }
}

// Internal function: Inserts an event into the red-black tree
static inline ws_event_handle *ws_insert_event(ws_event_base *we, ws_event_handle *item) {
    void **cp = rbProbe(we->events, item);
    if (!cp) {
        ws_log_error("Failed to insert event handle %lld into red-black tree.", item->id);
        return NULL;
    }
    if (*cp != item) {
        // Theoretically, this should not happen when using unique IDs as keys.
        // If it occurs, it means the generated ID is duplicated, or the tree's comparison function has issues.
        ws_log_error("Duplicate event ID %lld found in tree. Original: %p, New: %p", item->id, *cp, item);
        return NULL; // Return NULL indicating insertion failed or a duplicate was found
    }
    return item; // Successfully inserted
}

// Generic event internal callback (for normal I/O and time events)
static void ws_event_internal_cb(evutil_socket_t fd, short events, void *arg) {
    ws_event_handle *handle = (ws_event_handle *)arg;
    if (!handle) return;

    switch (handle->type) {
        case WS_EVENT_NORMAL:
            if (handle->normal.callback)
                handle->normal.callback(fd, events, handle->arg);
            // If it's a non-persistent event, delete it after the callback
            if (!(handle->normal.is_persistent)) {
                ws_event_del(handle);
            }
            break;
        case WS_EVENT_TIME:
            if (handle->time.callback)
                handle->time.callback(fd, events, handle->arg);
            // If it's a non-persistent time event, delete it after the callback
            if (!(handle->time.is_persistent)) {
                ws_event_del(handle);
            }
            break;
        default:
            ws_log_error("Unknown event type in internal_cb: %d", handle->type);
            break;
    }
}

// Internal function: Cleans up the HTTP request context
static void cleanup_http_context(ws_http_internal_context *ctx) {
    if (!ctx) return;

    if (ctx->timeout_ev) {
        event_del(ctx->timeout_ev);
        event_free(ctx->timeout_ev);
    }
    if (ctx->response_buffer) {
        evbuffer_free(ctx->response_buffer);
    }
    // Note: The lifecycle of req and conn is managed by libevent; no manual freeing here.
    // However, if ssl_obj failed before evhttp_connection_base_ssl_new took ownership,
    // it might not be managed by libevent. So in error paths, we free it manually.
    // In normal paths, evhttp_connection_base_ssl_new successfully takes ownership of ctx->ssl_obj.

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

        if (status_code >= 400 || status_code == 0) { // HTTP error code or no response code (may indicate connection issue)
            error_code = status_code == 0 ? -2 : status_code; // -2 for connection/protocol error, others are HTTP status codes
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
    }
    // Free internal context
    cleanup_http_context(ctx);
}

/**
 * @brief Creates a new ws_event_base structure.
 */
ws_event_base *ws_event_new(void) {
    ws_event_base *we = zmalloc(sizeof(ws_event_base));
    if (!we) {
        ws_log_error("Failed to allocate memory for ws_event_base.");
        return NULL;
    }

    we->base = event_base_new();
    if (!we->base) {
        ws_log_error("Failed to create libevent event_base.");
        zfree(we);
        return NULL;
    }

    // **Initialize OpenSSL library and create SSL_CTX**
    if (ws_ssl_init_libs() != 0) {
        ws_log_error("Failed to initialize OpenSSL libraries.");
        event_base_free(we->base);
        zfree(we);
        return NULL;
    }
    we->ssl_ctx = ws_ssl_client_ctx_new();
    if (!we->ssl_ctx) {
        ws_log_error("Failed to create SSL_CTX for ws_event_base.");
        ws_ssl_cleanup_libs(); // Clean up OpenSSL libraries
        event_base_free(we->base);
        zfree(we);
        return NULL;
    }

    we->next_event_id = 1; // ID starts from 1
    we->total_requests = 0;
    we->success_requests = 0;
    we->failed_requests = 0;

    we->events = rbCreate(ws_rb_comparison_func, NULL);
    if (!we->events) {
        ws_log_error("Failed to create red-black tree for events.");
        ws_ssl_free_ctx(we->ssl_ctx); // Clean up SSL_CTX
        ws_ssl_cleanup_libs();        // Clean up OpenSSL libraries
        event_base_free(we->base);
        zfree(we);
        return NULL;
    }

    return we;
}

/**
 * @brief Frees the ws_event_base structure and its associated resources.
 */
void ws_event_free(ws_event_base *we) {
    if (we) {
        if (we->events) {
            rbDestroy(we->events, ws_rb_item_func); // Destroy tree and free all remaining handles
        }
        if (we->base)
            event_base_free(we->base);
        // **Free SSL_CTX and clean up OpenSSL library**
        if (we->ssl_ctx) {
            ws_ssl_free_ctx(we->ssl_ctx);
        }
        ws_ssl_cleanup_libs();

        zfree(we);
    }
}

/**
 * @brief Deletes an event handle.
 */
void ws_event_del(ws_event_handle *handle) {
    if (!handle) return;

    if (handle->base && handle->base->events) {
        ws_log_info("Removing event handle ID %lld from red-black tree: %p", handle->id, (void*)handle);
        // rbDelete only removes the node from the tree and does not call ws_rb_item_func.
        // We manually call ws_rb_item_func to free the handle itself and libevent resources.
        rbDelete(handle->base->events, handle);
    }

    // Ensure libevent-related event objects are freed.
    // ws_rb_item_func already handles event_free; no need to repeat here.
    // However, for HTTP events, its internal context (ws_http_internal_context) is freed in the internal callback.
    ws_rb_item_func(handle, NULL);
}

/**
 * @brief Adds a normal event to the event loop.
 */
ws_event_handle *ws_event_add(ws_event_base *we, int fd,
    short events, ws_event_cb callback, void *arg, bool is_persistent) {
    ws_event_handle *h = NULL;
    short flags;

    if (!we || !we->base || !callback) {
        ws_log_error("Invalid parameters for ws_event_add: we=%p, callback=%p", (void*)we, (void*)callback);
        return NULL;
    }

    h = zcalloc(sizeof(ws_event_handle));
    if (!h) {
        ws_log_error("Failed to allocate memory for ws_event_handle (normal event).");
        return NULL;
    }

    h->id = we->next_event_id++; // Assign unique ID
    h->type = WS_EVENT_NORMAL;
    h->normal.callback = callback;
    h->arg = arg;
    h->normal.is_persistent = is_persistent;
    h->normal.fd = fd;
    h->base = we;

    flags = events;
    if (is_persistent) flags |= EV_PERSIST;

    h->normal.ev = event_new(we->base, fd, flags, ws_event_internal_cb, h);
    if (!h->normal.ev) {
        ws_log_error("Failed to create event for fd: %d, events: %d", fd, events);
        goto err;
    }

    if (event_add(h->normal.ev, NULL) < 0) {
        ws_log_error("Failed to add event for fd: %d, events: %d", fd, events);
        goto err;
    }

    if (ws_insert_event(we, h) == NULL) {
        ws_log_error("Failed to insert normal event handle %lld into tree.", h->id);
        goto err;
    }

    return h;
err:
    if (h) {
        if (h->normal.ev)
            event_free(h->normal.ev);
        zfree(h);
    }
    return NULL;
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
    SSL *ssl_obj = NULL; // **New: SSL object for HTTPS connections**

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
        if (scheme && strcmp(scheme, "https") == 0) {
            port = 443;
        } else {
            port = 80;
        }
    }

    // Build the full path (including query string)
    char full_path[2048];
    if (!path || strlen(path) == 0) {
        snprintf(full_path, sizeof(full_path), "/%s%s",
                 query ? "?" : "", query ? query : "");
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s%s",
                 path, query ? "?" : "", query ? query : "");
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

        // Use evhttp_connection_base_ssl_new to create HTTPS connection
        // Note: Upon success, libevent takes ownership of ssl_obj's lifecycle
        conn = evhttp_connection_base_ssl_new(we->base, NULL, host, port, ssl_obj);
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

    // Add custom headers
    if (options->headers) {
        struct evkeyval *header;
        TAILQ_FOREACH(header, options->headers, next) {
            evhttp_add_header(evhttp_request_get_output_headers(req), header->key, header->value);
        }
    }
    // Add default headers
    evhttp_add_header(evhttp_request_get_output_headers(req), "Host", host);
    evhttp_add_header(evhttp_request_get_output_headers(req), "Connection", "close"); // Simplified, no Keep-Alive

    // Handle POST/PUT data
    if ((options->method == WS_HTTP_POST || options->method == WS_HTTP_PUT)
        && options->body_data && options->body_len > 0) {
        struct evbuffer *output_buffer = evhttp_request_get_output_buffer(req);
        evbuffer_add(output_buffer, options->body_data, options->body_len);
        // Add default Content-Type if not provided
        if (!evhttp_find_header(evhttp_request_get_output_headers(req), "Content-Type")) {
            evhttp_add_header(evhttp_request_get_output_headers(req),
                              "Content-Type",
                              "application/x-www-form-urlencoded");
        }
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
// --- Error handling paths ---
err_req:
    if (req) evhttp_request_free(req); // Only if failed before evhttp_make_request took ownership
err_conn:
    if (conn) evhttp_connection_free(conn); // Only if failed before conn was associated with req
// Label to handle HTTPS errors where ssl_obj might need freeing if libevent didn't take ownership.
// This label is reached by 'goto err_ssl_obj_or_response_buffer' from the 'if (!conn)' block.
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

/**
 * @brief Adds a time-based event to the event loop.
 */
ws_event_handle *ws_event_add_time(ws_event_base *we,
    const struct timeval *tv, ws_event_cb callback, void *arg, bool is_persistent) {
    ws_event_handle *h = NULL;

    if (!we || !we->base || !tv || !callback) {
        ws_log_error("Invalid parameters for ws_event_add_time: we=%p, tv=%p, callback=%p", (void*)we, (void*)tv, (void*)callback);
        return NULL;
    }

    h = zcalloc(sizeof(ws_event_handle));
    if (!h) {
        ws_log_error("Failed to allocate memory for ws_event_handle (time event).");
        return NULL;
    }

    h->id = we->next_event_id++;
    h->type = WS_EVENT_TIME;
    h->time.callback = callback;
    h->time.timeout = *tv;
    h->arg = arg;
    h->base = we;
    h->time.is_persistent = is_persistent;

    short flags = EV_TIMEOUT;
    if (is_persistent) flags |= EV_PERSIST;

    h->time.ev = event_new(we->base, -1, flags, ws_event_internal_cb, h);
    if (!h->time.ev) {
        ws_log_error("Failed to create time event with timeout: %ld.%06ld", tv->tv_sec, tv->tv_usec);
        goto err;
    }

    if (event_add(h->time.ev, tv) < 0) {
        ws_log_error("Failed to add time event with timeout: %ld.%06ld", tv->tv_sec, tv->tv_usec);
        goto err;
    }

    if (ws_insert_event(we, h) == NULL) {
        ws_log_error("Failed to insert time event handle %lld into tree.", h->id);
        goto err;
    }

    return h;
err:
    if (h) {
        if (h->time.ev)
            event_free(h->time.ev);
        zfree(h);
    }
    return NULL;
}

/**
 * @brief Starts the event loop.
 */
int ws_event_loop(ws_event_base *we) {
    if (!we || !we->base)
        return -1;
    return event_base_dispatch(we->base);
}

/**
 * @brief Stops the event loop.
 */
void ws_event_stop(ws_event_base *we) {
    if (!we || !we->base)
        return;
    event_base_loopbreak(we->base);
}