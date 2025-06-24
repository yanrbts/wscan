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
#include <stdlib.h>
#include <string.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <core/ws_event.h>
#include <core/ws_log.h>

// 用户自定义的 HTTP 回调
void my_http_callback(struct evhttp_request *req, void *arg) {
    if (!req) {
        ws_log_error("HTTP request failed or timed out.");
        ws_event_base *we = (ws_event_base *)arg;
        ws_event_stop(we);
        return;
    }

    int code = evhttp_request_get_response_code(req);
    ws_log_info("HTTP response code: %d\n", code);

    struct evbuffer *buf = evhttp_request_get_input_buffer(req);
    if (buf) {
        size_t len = evbuffer_get_length(buf);
        char *data = malloc(len + 1);
        if (data) {
            evbuffer_copyout(buf, data, len);
            data[len] = '\0';
            printf("HTTP response body:\n%s\n", data);
            free(data);
        }
    }

    ws_event_base *we = (ws_event_base *)arg;
    ws_event_stop(we); // 停止事件循环
}

int main() {
    ws_event_base *we = ws_event_new();
    if (!we) {
        ws_log_error("Failed to create ws_event_base\n");
        return 1;
    }

    // 发起 HTTP GET 请求
    ws_event_handle *handle = ws_event_add_http(
        we,
        "httpbin.org", 80,
        "/get",
        my_http_callback,
        we
    );
    if (!handle) {
        ws_log_error("Failed to add HTTP event");
        ws_event_free(we);
        return 1;
    }

    ws_log_info("Starting event loop...");
    ws_event_loop(we);

    ws_event_free(we);
    ws_log_info("Done.");
    return 0;
}