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
#ifndef __WS_EVENT_H__
#define __WS_EVENT_H__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/util.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h> // For struct evkeyvalq
#include <ws_rbtree.h> // Assuming this is your red-black tree implementation

// 定义HTTP请求方法枚举
typedef enum {
    WS_HTTP_GET,
    WS_HTTP_POST,
    WS_HTTP_PUT,
    WS_HTTP_DELETE,
    // 您可以根据需要添加更多方法
} ws_http_method;

// HTTP响应回调函数类型
// 参数：
//   status_code: HTTP 状态码 (如 200, 404, 500)
//   headers: 响应头 (struct evkeyvalq 类型，可使用TAILQ_FOREACH遍历)
//   body_data: 响应体数据指针
//   body_len: 响应体数据长度
//   user_data: 用户传入的上下文数据
//   error_code: 如果请求失败，表示错误码 (0表示成功，-1超时，-2连接/协议错误，>=400为HTTP状态码错误)
typedef void (*ws_event_http_cb)(int status_code, struct evkeyvalq *headers,
                                 const char *body_data, size_t body_len,
                                 void *user_data, int error_code);

// 通用事件回调函数类型
typedef void (*ws_event_cb)(int fd, short events, void *arg);

// 事件类型枚举
typedef enum {
    WS_EVENT_NORMAL = 0,
    WS_EVENT_HTTP = 1,
    WS_EVENT_TIME = 2
} ws_event_type;

// 事件基结构体
typedef struct ws_event_base {
    struct event_base *base;
    long long next_event_id; // 用于生成唯一的事件ID
    int total_requests;
    int success_requests;
    int failed_requests;
    rbTable *events; // 红黑树来管理事件
} ws_event_base;

// 事件句柄结构体
typedef struct ws_event_handle {
    long long id;              // 唯一标识符，作为红黑树的键
    ws_event_type type;        // 事件类型 (普通, HTTP, 时间)
    ws_event_base *base;       // 指向其所属的事件基

    union {
        /* 普通事件 */
        struct {
            int fd;
            struct event *ev;
            ws_event_cb callback;
            bool is_persistent;
        } normal;
        /* HTTP事件 */
        struct {
            // 注意：http.conn 不再在 ws_event_handle 中显式保存
            // 它的生命周期由 libevent 管理，或在需要连接池时由连接池管理
            ws_event_http_cb callback;
            void *internal_ctx; // 指向内部的HTTP请求上下文
        } http;
        /* 时间事件 */
        struct {
            struct event *ev;
            ws_event_cb callback;
            struct timeval timeout;
            bool is_persistent;
        } time;
    };

    void *arg; // 用户传递给回调函数的参数
} ws_event_handle;

// HTTP 请求参数结构体 (取代原有的 host, port, uri 参数)
typedef struct ws_http_request_options {
    ws_http_method method;              // 请求方法 (GET, POST等)
    const char *url;                    // 完整URL (如 "http://example.com/path?query=val")
    struct evkeyvalq *headers;          // 可选的自定义请求头
    const char *body_data;              // POST/PUT 请求体数据 (可选)
    size_t body_len;                    // POST/PUT 请求体数据长度
    long timeout_ms;                    // 请求超时时间 (毫秒，0表示使用默认或无超时)
    bool follow_redirects;              // 是否自动跟随重定向 (需要额外的逻辑实现)
} ws_http_request_options;


/**
 * @brief 创建一个新的事件基。
 * 此函数初始化一个新的事件基，它是事件循环的核心。
 * 它为 ws_event_base 结构分配内存并设置事件基。
 * @return 指向新创建的 ws_event_base 结构的指针，失败时返回NULL。
 */
ws_event_base *ws_event_new(void);

/**
 * @brief 释放 ws_event 结构及其相关资源。
 * 此函数负责清理事件基并释放为 ws_event 结构分配的内存。
 * 它确保所有资源都被正确释放，以防止内存泄漏。
 * @param we 指向要释放的 ws_event_base 结构的指针。
 */
void ws_event_free(ws_event_base *we);

/**
 * @brief 删除一个事件句柄。
 * 此函数负责从事件循环中移除事件并释放相关资源。
 * 它确保不再监视事件并正确释放内存。
 * @param handle 指向要删除的 ws_event_handle 结构的指针。
 */
void ws_event_del(ws_event_handle *handle);

/**
 * @brief 向事件循环添加一个普通事件。
 * 此函数创建一个新事件，当文件描述符准备好读写时将触发。
 * 它允许异步I/O操作，这对于网络套接字或文件描述符很有用。
 * @param we 指向 ws_event_base 结构的指针。
 * @param fd 要监视的文件描述符。
 * @param events 要监视的事件 (例如，EV_READ, EV_WRITE)。
 * @param callback 事件触发时要调用的回调函数。
 * @param arg 传递给回调函数的参数。
 * @param is_persistent 如果为true，事件将是持久的，并将重复触发直到明确移除。
 * @return 指向新创建的 ws_event_handle 结构的指针，失败时返回NULL。
 */
ws_event_handle *ws_event_add(ws_event_base *we,
    int fd, short events,
    ws_event_cb callback, void *arg, bool is_persistent);

/**
 * @brief 向事件循环发送一个HTTP请求。
 * 此函数创建一个新的HTTP请求，并在请求完成后触发回调。
 * 它允许异步处理HTTP请求，这对于Web应用程序或服务很有用。
 * @param we 指向 ws_event_base 结构的指针。
 * @param options 指向包含请求配置的结构体。
 * @param callback HTTP请求完成时要调用的回调函数。
 * @param arg 传递给回调函数的参数。
 * @return 指向新创建的 ws_event_handle 结构的指针，成功启动请求时返回，失败时返回NULL。
 * 请求结果将在回调函数中异步返回。
 */
ws_event_handle *ws_event_http_request(ws_event_base *we,
    const ws_http_request_options *options,
    ws_event_http_cb callback, void *arg);

/**
 * @brief 向事件循环添加一个基于时间的事件。
 * 此函数创建一个新的基于时间的事件，将在指定超时后触发。
 * 它允许在一定时间后安排事件发生，这对于超时或周期性检查等任务很有用。
 * @param we 指向 ws_event_base 结构的指针。
 * @param tv 指向 timeval 结构的指针，指定超时持续时间。
 * @param callback 事件触发时要调用的回调函数。
 * @param arg 传递给回调函数的参数。
 * @param is_persistent 如果为true，事件将是持久的，并将重复触发直到明确移除。
 * @return 指向新创建的 ws_event_handle 结构的指针，失败时返回NULL。
 */
ws_event_handle *ws_event_add_time(ws_event_base *we,
    const struct timeval *tv, ws_event_cb callback, void *arg, bool is_persistent);

/**
 * @brief 启动事件循环。
 * 此函数启动事件循环，它将一直运行，直到没有更多事件要处理或循环被停止。
 * 它处理事件并在事件发生时调用关联的回调。
 * @param we 指向 ws_event_base 结构的指针。
 */
int ws_event_loop(ws_event_base *we);

/**
 * @brief 停止事件循环。
 * 此函数停止事件循环，使其能够优雅地退出。
 * 通常在应用程序关闭或不再需要处理事件时调用。
 * @param we 指向 ws_event_base 结构的指针。
 */
void ws_event_stop(ws_event_base *we);

#endif // __WS_EVENT_H__