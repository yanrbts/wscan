/*
 * Copyright (c) 2025-2025, yanruibinghxu@gmail.com All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
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
// test_http.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <core/ws_event.h>
#include <core/ws_log.h>
#include <ws_malloc.h>
#include <ws_util.h>
#include <ws_http.h>

// 普通I/O事件回调示例 (例如，从stdin读取)
void stdin_read_cb(int fd, short events, void *arg) {
    char buf[256];
    ssize_t nbytes = read(fd, buf, sizeof(buf) - 1);
    if (nbytes > 0) {
        buf[nbytes] = '\0';
        ws_log_info("Read from stdin (FD %d): %s", fd, buf);
        if (strstr(buf, "quit")) {
            ws_event_base *we = (ws_event_base *)arg;
            ws_log_info("Stopping event loop due to 'quit' command.");
            ws_event_stop(we);
        }
    } else if (nbytes == 0) {
        ws_log_info("Stdin (FD %d) closed. Deleting event.", fd);
        ws_event_del((ws_event_handle *)arg); // 从树中删除自身
    } else {
        ws_log_error("Error reading from stdin (FD %d): %s", fd, strerror(errno));
        ws_event_del((ws_event_handle *)arg);
    }
}

// HTTP响应处理回调函数
void my_http_response_handler(int status_code, struct evkeyvalq *headers,
                              const char *body_data, size_t body_len,
                              void *user_data, int error_code) {
    ws_event_base *we = (ws_event_base *)user_data; // 假设 user_data 是 ws_event_base

    if (error_code == 0) {
        ws_log_info("--- HTTP Response (Success) ---");
        ws_log_info("Status Code: %d", status_code);

        ws_log_info("Headers:");
        if (headers) {
            struct evkeyval *header;
            TAILQ_FOREACH(header, headers, next) {
                ws_log_info("  %s: %s", header->key, header->value);
            }
        }

        ws_log_info("Body (length %zu):", body_len);
        if (body_data) {
            // 为避免打印大量二进制数据，只打印文本部分或截断
            if (body_len > 500) { // 避免大量输出
                printf("%.*s...\n", 500, body_data);
            } else {
                printf("%.*s\n", (int)body_len, body_data);
            }
        } else {
            ws_log_info("[No body]");
        }
    } else {
        ws_log_error("--- HTTP Request Failed! ---");
        ws_log_error("Error Code: %d", error_code);
        if (error_code == -1) {
            ws_log_error("Reason: Request Timed Out.");
        } else if (error_code == -2) {
            ws_log_error("Reason: Connection/Protocol Error.");
        } else if (error_code >= 400) {
            ws_log_error("Reason: HTTP Status Code Error (HTTP %d).", status_code);
        }
    }

    // 打印当前统计信息
    ws_log_info("Requests Stats: Total=%d, Success=%d, Failed=%d",
                we->total_requests, we->success_requests, we->failed_requests);

    // 如果所有请求都已完成，停止事件循环 (根据你的应用逻辑决定何时停止)
    // 这里我们简单地在每个请求完成后检查，如果这是最后一个预期请求，就停止。
    // 更复杂的应用会根据业务逻辑决定。
    // event_base_loopbreak(we->base); // 在实际应用中，你可能不会在这里停止，而是等待更多事件
}

// 定时器事件回调示例
void my_timer_cb(int fd, short events, void *arg) {
    UNUSED(fd);
    UNUSED(events);
    ws_log_info("Timer triggered! Arg: %s", (char *)arg);
    // 如果定时器是非持久的，它会在回调后自动被 ws_event_internal_cb 删除
}


int main() {
    // 1. 初始化事件基
    ws_event_base *we = ws_event_new();
    if (!we) {
        fprintf(stderr, "Could not initialize ws_event_base!\n");
        return 1;
    }

    ws_log_info("ws_event_base initialized.");

    // // 2. 添加一个普通I/O事件：监听标准输入
    // ws_event_handle *stdin_handle = ws_event_add(we, STDIN_FILENO, EV_READ | EV_PERSIST, stdin_read_cb, we, true);
    // if (!stdin_handle) {
    //     ws_log_error("Failed to add stdin event.");
    // } else {
    //     ws_log_info("Added stdin monitor event (ID: %lld). Type 'quit' to exit.", stdin_handle->id);
    // }

    // 3. 添加一个时间事件：5秒后触发一次性定时器
    struct timeval tv_one_shot = {5, 0}; // 5秒
    ws_event_handle *timer_one_shot_handle = ws_event_add_time(we, &tv_one_shot, my_timer_cb, (void*)"One-shot timer", true);
    if (!timer_one_shot_handle) {
        ws_log_error("Failed to add one-shot timer event.");
    } else {
        ws_log_info("Added one-shot timer event (ID: %lld) to trigger in 5 seconds.", timer_one_shot_handle->id);
    }
    
    // 4. 发送一个 GET HTTP请求
    ws_http_request_options get_options = {0};
    get_options.method = WS_HTTP_GET;
    get_options.url = "http://httpbin.org/get?param1=value1"; // 一个简单的GET请求
    get_options.timeout_ms = 8000; // 8秒超时

    ws_log_info("Sending GET request to %s", get_options.url);
    ws_event_handle *get_req_handle = ws_event_http_request(we, &get_options, my_http_response_handler, we);
    if (!get_req_handle) {
        ws_log_error("Failed to send GET request.");
    } else {
        ws_log_info("GET request sent (Handle ID: %lld).", get_req_handle->id);
    }
    
    // 5. 发送一个 POST HTTP请求 (带数据和自定义头部)
    char *post_data = "";
    struct evkeyvalq *post_headers = zmalloc(sizeof(struct evkeyvalq));
    TAILQ_INIT(post_headers);
    evhttp_add_header(post_headers, "X-Custom-Header", "MyValue");
    evhttp_add_header(post_headers, "Content-Type", "application/x-www-form-urlencoded"); // 通常POST需要设置

    ws_http_request_options post_options = {0};
    post_options.method = WS_HTTP_GET;
    post_options.url = "https://www.baidu.com"; // 会返回POST的数据
    post_options.body_data = post_data;
    post_options.body_len = strlen(post_data);
    post_options.headers = post_headers;
    post_options.timeout_ms = 8000; // 8秒超时

    ws_log_info("Sending POST request to %s with data: %s", post_options.url, post_data);
    ws_event_handle *post_req_handle = ws_event_http_request(we, &post_options, my_http_response_handler, we);
    if (!post_req_handle) {
        ws_log_error("Failed to send POST request.");
    } else {
        ws_log_info("POST request sent (Handle ID: %lld).", post_req_handle->id);
    }
    // 释放 post_headers，因为 evhttp_request_new 会复制这些头部
    evhttp_clear_headers(post_headers);
    zfree(post_headers);

    // 6. 运行事件循环
    ws_log_info("Starting event loop...");
    ws_event_loop(we); // 阻塞直到所有事件完成或 ws_event_stop 被调用

    ws_log_info("Event loop stopped.");

    // 7. 清理资源
    ws_event_free(we);
    ws_log_info("ws_event_base freed. Program finished.");

    return 0;
}