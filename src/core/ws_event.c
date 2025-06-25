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
#include <ws_malloc.h> // 假设您的内存分配函数
#include <ws_log.h>    // 假设您的日志函数
#include <ws_event.h>
#include <ws_util.h>   // 假设您的实用工具函数，例如 UNUSED 宏
#include <string.h>
#include <sys/queue.h>

// 内部使用的HTTP请求上下文结构体
typedef struct _ws_http_internal_context {
    ws_event_handle *handle;           // 指向外部 ws_event_handle
    struct evhttp_request *req;        // libevent HTTP请求对象
    struct evhttp_connection *conn;    // libevent HTTP连接对象 (可能被重用或由libevent管理)
    struct event *timeout_ev;          // 超时事件
    ws_event_http_cb callback;         // 用户提供的HTTP回调
    void *user_data;                   // 用户传递的参数
    struct evbuffer *response_buffer;  // 用于积累响应体数据
} _ws_http_internal_context;

// 红黑树比较函数：现在使用事件句柄的唯一ID进行比较
static int ws_rb_comparison_func(const void *rb_a, const void *rb_b, void *rb_param) {
    UNUSED(rb_param);
    const ws_event_handle *a = (const ws_event_handle *)rb_a;
    const ws_event_handle *b = (const ws_event_handle *)rb_b;

    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0; // ID相同，理论上不应该发生
}

// 红黑树项释放函数：仅释放 ws_event_handle 本身及其 libevent 事件对象
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
                // HTTP事件的内部上下文在 _http_request_done 或超时时释放
                // 确保 h->http.internal_ctx 在此时已经通过 _cleanup_http_context 释放
                // 这里只释放 ws_event_handle 本身
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
        // 释放事件句柄本身
        zfree(h);
    }
}

// 内部函数：插入事件到红黑树
static inline ws_event_handle *ws_insert_event(ws_event_base *we, ws_event_handle *item) {
    void **cp = rbProbe(we->events, item);
    if (!cp) {
        ws_log_error("Failed to insert event handle %lld into red-black tree.", item->id);
        return NULL;
    }
    if (*cp != item) {
        // 理论上，使用唯一ID作为键时，不应该出现这种情况
        // 如果出现，意味着生成的ID重复，或者树的比较函数有问题
        ws_log_error("Duplicate event ID %lld found in tree. Original: %p, New: %p", item->id, *cp, item);
        return NULL; // 返回NULL表示插入失败或发现重复项
    }
    return item; // 成功插入
}

// 通用事件内部回调 (用于普通I/O和时间事件)
static void ws_event_internal_cb(evutil_socket_t fd, short events, void *arg) {
    ws_event_handle *handle = (ws_event_handle *)arg;
    if (!handle) return;

    switch (handle->type) {
        case WS_EVENT_NORMAL:
            if (handle->normal.callback)
                handle->normal.callback(fd, events, handle->arg);
            // 如果是非持久事件，在回调后删除
            if (!(handle->normal.is_persistent)) {
                ws_event_del(handle);
            }
            break;
        case WS_EVENT_TIME:
            if (handle->time.callback)
                handle->time.callback(fd, events, handle->arg);
            // 如果是非持久时间事件，在回调后删除
            if (!(handle->time.is_persistent)) {
                ws_event_del(handle);
            }
            break;
        default:
            ws_log_error("Unknown event type in internal_cb: %d", handle->type);
            break;
    }
}

// 内部函数：清理HTTP请求上下文
static void _cleanup_http_context(_ws_http_internal_context *ctx) {
    if (!ctx) return;

    if (ctx->timeout_ev) {
        event_del(ctx->timeout_ev);
        event_free(ctx->timeout_ev);
    }
    if (ctx->response_buffer) {
        evbuffer_free(ctx->response_buffer);
    }
    // 注意：req 和 conn 的生命周期由 libevent 管理，
    // 特别是当 evhttp_make_request 成功后，req 由 libevent 内部接管。
    // connection 可能会被 libevent 重用或在 req 释放时一同释放。
    // 因此这里通常不需要手动 free evhttp_request 和 evhttp_connection。
    // 只有当 evhttp_make_request 失败时才需要手动 free req。

    zfree(ctx);
}

// HTTP请求超时回调
static void _http_request_timeout_cb(evutil_socket_t fd, short what, void *arg) {
    UNUSED(fd);
    UNUSED(what);
    _ws_http_internal_context *ctx = (_ws_http_internal_context *)arg;
    if (!ctx) return;

    ws_log_error("HTTP Request ID %lld timed out for handle %p.", ctx->handle->id, (void*)ctx->handle);

    // 增加失败计数
    ctx->handle->base->failed_requests++;

    // 调用用户回调并传递超时错误
    if (ctx->callback) {
        ctx->callback(0, NULL, NULL, 0, ctx->user_data, -1); // -1 表示超时错误
    }

    // 确保从 ws_event_base 的红黑树中删除此 handle
    if (ctx->handle) {
        ws_event_del(ctx->handle); // 这会触发 ws_rb_item_func 释放 ws_event_handle
    }
    // 释放内部上下文
    _cleanup_http_context(ctx);
}

// HTTP请求完成回调 (由 libevent 调用)
static void ws_event_internal_http_cb(struct evhttp_request *req, void *arg) {
    _ws_http_internal_context *ctx = (_ws_http_internal_context *)arg;
    if (!ctx) return;

    // 请求已完成，取消超时事件
    if (ctx->timeout_ev) {
        event_del(ctx->timeout_ev);
    }

    int status_code = 0;
    const char *body_data = NULL;
    size_t body_len = 0;
    int error_code = 0; // 0表示成功

    if (req) {
        status_code = evhttp_request_get_response_code(req);
        struct evbuffer *buf = evhttp_request_get_input_buffer(req);
        if (buf) {
            body_len = evbuffer_get_length(buf);
            body_data = (const char *)evbuffer_pullup(buf, body_len);
        }

        if (status_code >= 400 || status_code == 0) { // HTTP错误码或无响应码(可能表示连接问题)
            error_code = status_code == 0 ? -2 : status_code; // -2表示连接/协议错误，其他是HTTP错误码
            ctx->handle->base->failed_requests++;
            ws_log_error("HTTP request ID %lld to %s failed with status %d, error code %d",
                         ctx->handle->id, evhttp_request_get_uri(req), status_code, error_code);
        } else {
            ctx->handle->base->success_requests++;
            ws_log_info("HTTP request ID %lld to %s completed with status %d.",
                        ctx->handle->id, evhttp_request_get_uri(req), status_code);
        }
    } else {
        // 请求失败，例如连接错误、DNS解析失败等
        ws_log_error("HTTP request ID %lld failed (no response or connection error).", ctx->handle->id);
        error_code = -2; // 例如，-2表示连接或协议错误
        ctx->handle->base->failed_requests++;
    }

    ctx->handle->base->total_requests++;

    // 调用用户回调
    if (ctx->callback) {
        ctx->callback(status_code, req ? evhttp_request_get_input_headers(req) : NULL,
                      body_data, body_len, ctx->user_data, error_code);
    }

    // 从 ws_event_base 的红黑树中删除此 handle
    // ws_event_del 会触发 ws_rb_item_func 来释放 ws_event_handle
    if (ctx->handle) {
        ws_event_del(ctx->handle);
    }
    // 释放内部上下文
    _cleanup_http_context(ctx);
}

/**
 * @brief 创建一个新的 ws_event_base 结构。
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

    we->next_event_id = 1; // ID从1开始
    we->total_requests = 0;
    we->success_requests = 0;
    we->failed_requests = 0;

    we->events = rbCreate(ws_rb_comparison_func, NULL);
    if (!we->events) {
        ws_log_error("Failed to create red-black tree for events.");
        event_base_free(we->base);
        zfree(we);
        return NULL;
    }

    return we;
}

/**
 * @brief 释放 ws_event_base 结构及其相关资源。
 */
void ws_event_free(ws_event_base *we) {
    if (we) {
        if (we->events) {
            rbDestroy(we->events, ws_rb_item_func); // 销毁树并释放所有剩余的handle
        }
        if (we->base)
            event_base_free(we->base);
        zfree(we);
    }
}

/**
 * @brief 删除一个事件句柄。
 */
void ws_event_del(ws_event_handle *handle) {
    if (!handle) return;

    if (handle->base && handle->base->events) {
        ws_log_info("Removing event handle ID %lld from red-black tree: %p", handle->id, (void*)handle);
        // rbDelete 只从树中移除节点，不调用 ws_rb_item_func，
        // 我们手动调用 ws_rb_item_func 来释放 handle 自身和 libevent 资源
        rbDelete(handle->base->events, handle);
    }

    // 确保释放 libevent 相关的事件对象
    // ws_rb_item_func 已经处理了 event_free，这里不再重复处理
    // 但对于 HTTP 事件，其内部上下文 (_ws_http_internal_context) 在内部回调中释放
    ws_rb_item_func(handle, NULL);
}

/**
 * @brief 向事件循环添加一个普通事件。
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

    h->id = we->next_event_id++; // 分配唯一ID
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
 * @brief 向事件循环发送一个HTTP请求。
 */
ws_event_handle *ws_event_http_request(ws_event_base *we,
    const ws_http_request_options *options,
    ws_event_http_cb callback, void *arg) {

    struct evhttp_connection *conn = NULL;
    struct evhttp_request *req = NULL; // 保持为NULL，由libevent管理生命周期
    ws_event_handle *h = NULL;
    _ws_http_internal_context *internal_ctx = NULL;
    struct evhttp_uri *uri = NULL; // libevent内置的URI解析器

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
    const char *query = evhttp_uri_get_query(uri); // 获取查询字符串部分

    if (!host) {
        ws_log_error("URL has no host: %s", options->url);
        goto err_uri;
    }

    // 处理端口默认值
    if (port == -1) {
        if (scheme && strcmp(scheme, "https") == 0) {
            port = 443;
        } else {
            port = 80;
        }
    }

    // 构建完整的路径 (包含查询字符串)
    char full_path[2048]; // 注意缓冲区大小，生产环境可能需要动态分配
    if (!path || strlen(path) == 0) {
        snprintf(full_path, sizeof(full_path), "/%s%s",
                 query ? "?" : "", query ? query : "");
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s%s",
                 path, query ? "?" : "", query ? query : "");
    }
    full_path[sizeof(full_path) - 1] = '\0'; // 确保字符串以空字符结尾

    h = zcalloc(sizeof(ws_event_handle));
    if (!h) {
        ws_log_error("Failed to allocate memory for ws_event_handle (HTTP event).");
        goto err_uri;
    }

    internal_ctx = zcalloc(sizeof(_ws_http_internal_context));
    if (!internal_ctx) {
        ws_log_error("Failed to allocate memory for internal HTTP context.");
        goto err_handle;
    }

    h->id = we->next_event_id++; // 分配唯一ID
    h->type = WS_EVENT_HTTP;
    h->base = we;
    h->http.callback = callback;
    h->arg = arg; // 用户参数
    h->http.internal_ctx = internal_ctx; // 关联内部上下文

    internal_ctx->handle = h;
    internal_ctx->callback = callback;
    internal_ctx->user_data = arg;
    internal_ctx->response_buffer = evbuffer_new(); // 创建响应体缓冲区
    if (!internal_ctx->response_buffer) {
        ws_log_error("Failed to create response buffer for HTTP request ID %lld.", h->id);
        goto err_internal_ctx;
    }

    // HTTPS 支持 (简化，需要完善SSL上下文管理)
    if (scheme && strcmp(scheme, "https") == 0) {
        // 这是一个复杂的主题，需要OpenSSL相关初始化和配置SSL_CTX/SSL对象
        // conn = evhttp_connection_base_ssl_new(we->base, NULL, ..., host, port, ssl_ctx);
        ws_log_error("HTTPS support not fully implemented in this example. Falling back to HTTP or failing.");
        goto err_response_buffer;
    } else {
        conn = evhttp_connection_base_new(we->base, NULL, host, port);
    }

    if (!conn) {
        ws_log_error("Failed to create HTTP connection for %s:%d.", host, port);
        goto err_response_buffer;
    }
    internal_ctx->conn = conn; // 存储连接对象，但不管理其生命周期

    // 创建HTTP请求
    req = evhttp_request_new(ws_event_internal_http_cb, internal_ctx);
    if (!req) {
        ws_log_error("Failed to create HTTP request for URL: %s.", options->url);
        goto err_conn; // req创建失败，conn需要被释放
    }
    internal_ctx->req = req; // 存储请求对象，但由libevent管理生命周期

    // 设置请求超时 (libevent以秒为单位)
    if (options->timeout_ms > 0) {
        // evhttp_request_set_timeout(req, options->timeout_ms / 1000);
        // 也可以为 evhttp_connection 设置超时，但 evhttp_request_set_timeout 更直接针对请求
        // evhttp_connection_set_timeout(conn, options->timeout_ms / 1000);
        // 使用一个独立的超时事件，以更精确地触发超时回调，即使libevent内部请求可能没有立即失败
        struct timeval tv;
        tv.tv_sec = options->timeout_ms / 1000;
        tv.tv_usec = (options->timeout_ms % 1000) * 1000;
        internal_ctx->timeout_ev = event_new(we->base, -1, EV_TIMEOUT, _http_request_timeout_cb, internal_ctx);
        if (internal_ctx->timeout_ev) {
            event_add(internal_ctx->timeout_ev, &tv);
        } else {
            ws_log_error("Warning: Could not create timeout event for HTTP request ID %lld.", h->id);
        }
    }

    // 添加自定义头部
    if (options->headers) {
        struct evkeyval *header;
        TAILQ_FOREACH(header, options->headers, next) {
            evhttp_add_header(evhttp_request_get_output_headers(req), header->key, header->value);
        }
    }
    // 添加默认头部
    evhttp_add_header(evhttp_request_get_output_headers(req), "Host", host);
    evhttp_add_header(evhttp_request_get_output_headers(req), "Connection", "close"); // 简化，不使用Keep-Alive

    // 处理POST/PUT数据
    if ((options->method == WS_HTTP_POST || options->method == WS_HTTP_PUT) &&
        options->body_data && options->body_len > 0) {
        struct evbuffer *output_buffer = evhttp_request_get_output_buffer(req);
        evbuffer_add(output_buffer, options->body_data, options->body_len);
        // 如果未提供Content-Type，则添加默认值
        if (!evhttp_find_header(evhttp_request_get_output_headers(req), "Content-Type")) {
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/x-www-form-urlencoded");
        }
    }

    // 设置HTTP方法
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

    // 发送请求
    if (evhttp_make_request(conn, req, ev_http_method, full_path) == -1) {
        ws_log_error("Failed to make HTTP request to %s.", options->url);
        goto err_req; // evhttp_make_request 失败时需要手动释放 req
    }

    // 将事件句柄插入到红黑树中
    if (ws_insert_event(we, h) == NULL) {
        ws_log_error("Failed to insert HTTP event handle %lld into red-black tree. Request might still complete.", h->id);
        // 注意：请求可能已经发出，但句柄未被树管理。
        // 此时我们无法简单地清理 req 和 conn，因为它们已由 libevent 接管。
        // h 的生命周期将由 _http_request_done 或 _http_request_timeout_cb 来管理。
        // 此处返回 NULL 表示“启动”失败（但网络请求可能已发送）。
        evhttp_uri_free(uri);
        return NULL;
    }

    evhttp_uri_free(uri); // 释放URI对象
    return h; // 成功启动请求并插入句柄
// --- 错误处理路径 ---
err_req:
    if (req) evhttp_request_free(req); // 只有在 evhttp_make_request 之前失败才需要
err_conn:
    if (conn) evhttp_connection_free(conn); // 只有在 conn 未与 req 关联时才需要
err_response_buffer:
    if (internal_ctx && internal_ctx->response_buffer) evbuffer_free(internal_ctx->response_buffer);
err_internal_ctx:
    if (internal_ctx) zfree(internal_ctx);
err_handle:
    if (h) zfree(h);
err_uri:
    if (uri) evhttp_uri_free(uri);
    return NULL;
}

/**
 * @brief 向事件循环添加一个基于时间的事件。
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

    h->id = we->next_event_id++; // 分配唯一ID
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
 * @brief 启动事件循环。
 */
int ws_event_loop(ws_event_base *we) {
    if (!we || !we->base)
        return -1;
    return event_base_dispatch(we->base);
}

/**
 * @brief 停止事件循环。
 */
void ws_event_stop(ws_event_base *we) {
    if (!we || !we->base)
        return;
    event_base_loopbreak(we->base);
}