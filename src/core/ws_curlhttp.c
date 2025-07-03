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
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For close()
#include <assert.h> // For assert
#include <stdio.h>  // For printf, fprintf
#include <event2/http.h>
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_curlhttp.h>

// libcurl write data callback
static size_t ws_write_data_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    ws_response *response = (ws_response *)userdata;
    size_t new_len = response->content_len + size * nmemb;

    char *new_content = zrealloc(response->content, new_len + 1);
    if (new_content == NULL) {
        fprintf(stderr, "realloc failed in write_data_callback\n");
        return 0; // Return 0 to signal an error to libcurl
    }
    response->content = new_content;
    memcpy(response->content + response->content_len, ptr, size * nmemb);
    response->content[new_len] = '\0'; // Null-terminate
    response->content_len = new_len;
    return size * nmemb;
}

// libcurl header data callback
static size_t ws_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    ws_response *response = (ws_response *)userdata;
    size_t len = size * nitems;

    // Headers are in "Name: Value\r\n" format
    // Extract name and value and add to evkeyvalq
    if (len > 2) { // Minimum "N:\r\n"
        char *line = ws_safe_strdup(buffer);
        if (line) {
            line[len - 2] = '\0'; // Remove \r\n
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0'; // Null-terminate name
                char *name = line;
                char *value = colon + 1;
                while (*value == ' ') value++; // Skip leading spaces for value
                evhttp_add_header(&response->headers, name, value);
            }
            zfree(line);
        }
    }
    return len;
}

// Forward declaration for ws_check_multi_info
static void ws_check_multi_info(ws_http_manager_ctx *manager_ctx);

// ws_event callback for socket activity (read/write)
static void ws_curl_socket_event_cb(int fd, short events, void *arg) {
    ws_http_request_context *ctx = (ws_http_request_context *)arg;
    ws_http_manager_ctx *manager_ctx = ctx->manager_ctx;
    int still_running;

    // Translate libevent events to CURL_CSELECT bits
    int curl_events = 0;
    if (events & EV_READ) curl_events |= CURL_CSELECT_IN;
    if (events & EV_WRITE) curl_events |= CURL_CSELECT_OUT;

    CURLMcode mc = curl_multi_socket_action(ctx->multi_handle, fd, curl_events, &still_running);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_socket_action failed: %s\n", curl_multi_strerror(mc));
        // Error handling: The request will eventually be marked as failed in ws_check_multi_info
    }

    // After curl_multi_socket_action, check for completed transfers
    ws_check_multi_info(manager_ctx);
}

// libcurl socket callback, registers FDs with ws_event (libevent)
static int ws_socket_callback(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp) {
    // userp is the CURLMOPT_SOCKETDATA which is ws_http_manager_ctx*
    ws_http_manager_ctx *manager_ctx = (ws_http_manager_ctx *)userp;
    ws_event_base *we = manager_ctx->event_base;

    // socketp is the private data for this socket, which should be the ws_http_request_context*
    // It's set by ws_http_perform_request when adding the easy handle.
    ws_http_request_context *ctx = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char**)&ctx); // Retrieve our context
    if (!ctx) {
        ws_log_error("Error: ws_http_request_context not found for socket %d.\n", s);
        return 1; // Indicate error to libcurl
    }

    if (what == CURL_POLL_REMOVE) {
        // Remove existing events for this socket
        if (ctx->socket_event_read) {
            ws_event_del(ctx->socket_event_read);
            ctx->socket_event_read = NULL;
        }
        if (ctx->socket_event_write) {
            ws_event_del(ctx->socket_event_write);
            ctx->socket_event_write = NULL;
        }
    } else {
        // Prepare events for libevent
        // Note: For libevent, calling `event_add` on an existing event object updates it.
        // We are using `ws_event_add` which is designed to handle this (it frees/recreates libevent's `struct event`).
        // However, if we need to track separate read/write handles, it's safer to have two distinct `ws_event_handle`s.

        if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
            if (!ctx->socket_event_read) {
                // Create a new persistent event for reading
                ctx->socket_event_read = ws_event_add(we, s, EV_READ, ws_curl_socket_event_cb, ctx, true);
            } else {
                // Event already exists, re-add to update/ensure it's active
                // ws_event_add will delete and re-create the underlying libevent event for robustness.
                ws_event_add(we, s, EV_READ, ws_curl_socket_event_cb, ctx, true);
            }
        } else if (ctx->socket_event_read) { // No longer interested in read
             ws_event_del(ctx->socket_event_read);
             ctx->socket_event_read = NULL;
        }

        if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
            if (!ctx->socket_event_write) {
                // Create a new persistent event for writing
                ctx->socket_event_write = ws_event_add(we, s, EV_WRITE, ws_curl_socket_event_cb, ctx, true);
            } else {
                // Event already exists, re-add to update/ensure it's active
                ws_event_add(we, s, EV_WRITE, ws_curl_socket_event_cb, ctx, true);
            }
        } else if (ctx->socket_event_write) { // No longer interested in write
            ws_event_del(ctx->socket_event_write);
            ctx->socket_event_write = NULL;
        }
    }
    return 0;
}

// ws_event callback for multi-handle timer
static void ws_curl_multi_timer_cb(int fd, short events, void *arg) {
    (void)fd; // Unused
    (void)events; // Unused
    ws_http_manager_ctx *manager_ctx = (ws_http_manager_ctx *)arg;
    int still_running;

    // This callback is triggered when libcurl's internal timer expires.
    CURLMcode mc = curl_multi_socket_action(manager_ctx->multi_handle, CURL_SOCKET_TIMEOUT, 0, &still_running);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_socket_action (timeout) failed: %s\n", curl_multi_strerror(mc));
    }
    // After curl_multi_socket_action, check for completed transfers
    ws_check_multi_info(manager_ctx);
}

// libcurl timer callback, sets/clears timers in ws_event (libevent)
static int ws_timer_callback(CURLM *multi, long timeout_ms, void *userp) {
    ws_http_manager_ctx *manager_ctx = (ws_http_manager_ctx *)userp;
    ws_event_base *we = manager_ctx->event_base;

    if (timeout_ms < 0) { // Timeout disabled
        if (manager_ctx->curl_multi_timer_event_handle) {
            ws_log_info("Disabling curl multi timer event.");
            ws_event_del(manager_ctx->curl_multi_timer_event_handle);
            manager_ctx->curl_multi_timer_event_handle = NULL;
        }
    } else { // Set/update timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (manager_ctx->curl_multi_timer_event_handle) {
            // Timer event already exists, just update it by re-adding (libevent handles this)
            // It's safe to call event_add again on the same event struct.
            // ws_event_add_time will internally call event_del and event_add for robustness.
            ws_log_info("Updating existing curl multi timer event with timeout %ld.%06ld seconds.", tv.tv_sec, tv.tv_usec);
            manager_ctx->curl_multi_timer_event_handle = ws_event_add_time(we, &tv, ws_curl_multi_timer_cb, manager_ctx, false); // Not persistent
        } else {
            // Create new timer event
            ws_log_info("Setting new curl multi timer event with timeout %ld.%06ld seconds.", tv.tv_sec, tv.tv_usec);
            manager_ctx->curl_multi_timer_event_handle = ws_event_add_time(we, &tv, ws_curl_multi_timer_cb, manager_ctx, false); // Not persistent
            if (!manager_ctx->curl_multi_timer_event_handle) {
                ws_log_error("Failed to add curl multi timer event.");
            }
        }
    }
    return 0;
}

// Function to process completed transfers from multi_info_read
static void ws_check_multi_info(ws_http_manager_ctx *manager_ctx) {
    CURLM *multi_handle = manager_ctx->multi_handle;
    ws_event_base *we = manager_ctx->event_base;
    CURLMsg *msg;
    int msgs_left;

    while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            CURL *easy_handle = msg->easy_handle;
            ws_http_request_context *ctx;
            curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, (char**)&ctx); // Get our context

            if (ctx) {
                // Populate response data
                long response_code;
                curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
                ctx->original_request->response->status_code = response_code;

                char *effective_url = NULL;
                curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &effective_url);
                ctx->original_request->response->url = ws_safe_strdup(effective_url);

                if (msg->data.result != CURLE_OK) {
                    ctx->original_request->response->error_message = ws_safe_strdup(curl_easy_strerror(msg->data.result));
                    ws_log_error("Request failed for %s: %s\n", effective_url, ctx->original_request->response->error_message);
                    we->failed_requests++;
                } else {
                    we->success_requests++;
                }

                // Invoke the completion callback
                if (ctx->completion_callback) {
                    ctx->completion_callback(
                        ctx->original_request->response->status_code,
                        &ctx->original_request->response->headers,
                        ctx->original_request->response->content,
                        ctx->original_request->response->content_len,
                        ctx->original_request, // Pass the original request as user_data
                        (int)msg->data.result // Pass CURLcode as error_code for more detail, 0 for success
                    );
                }

                // Cleanup: remove from multi handle, cleanup easy handle
                curl_multi_remove_handle(multi_handle, easy_handle);
                curl_easy_cleanup(easy_handle);

                // Free the internal context and its associated ws_event_handles
                // These handles were already removed/freed by ws_socket_callback with CURL_POLL_REMOVE
                // or if the connection was closed. Double check this logic.
                // A safer way is to just call ws_event_del here if handles are still set.
                if (ctx->socket_event_read) {
                    ws_event_del(ctx->socket_event_read);
                    ctx->socket_event_read = NULL;
                }
                if (ctx->socket_event_write) {
                    ws_event_del(ctx->socket_event_write);
                    ctx->socket_event_write = NULL;
                }
                curl_slist_free_all(ctx->current_headers_list); // Free the headers list if it was created
                zfree(ctx); // Free the context itself. ws_Request will be freed by explorer.
            } else {
                ws_log_error("Warning: Context not found for completed easy handle.\n");
                // Cleanup orphaned easy handle if somehow context is lost
                curl_multi_remove_handle(multi_handle, easy_handle);
                curl_easy_cleanup(easy_handle);
            }
        }
    }
}

ws_http_manager_ctx* ws_http_init(ws_event_base *we) {
    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle) return NULL;

    ws_http_manager_ctx *manager_ctx = (ws_http_manager_ctx*)zcalloc(sizeof(ws_http_manager_ctx));
    if (!manager_ctx) {
        curl_multi_cleanup(multi_handle);
        return NULL;
    }
    // Initialize the manager context
    manager_ctx->multi_handle = multi_handle;
    manager_ctx->event_base = we;
    manager_ctx->curl_multi_timer_event_handle = NULL;                              // Initially no timer event

    // Set libcurl callbacks to integrate with ws_event
    curl_multi_setopt(multi_handle, CURLMOPT_SOCKETFUNCTION, ws_socket_callback);
    curl_multi_setopt(multi_handle, CURLMOPT_SOCKETDATA, manager_ctx);              // Pass manager context
    curl_multi_setopt(multi_handle, CURLMOPT_TIMERFUNCTION, ws_timer_callback);
    curl_multi_setopt(multi_handle, CURLMOPT_TIMERDATA, manager_ctx);               // Pass manager context

    curl_global_init(CURL_GLOBAL_ALL);                                              // Initialize curl globally once

    return manager_ctx;
}

void ws_http_cleanup(ws_http_manager_ctx *manager_ctx) {
    if (!manager_ctx) return;

    if (manager_ctx->multi_handle) {
        // Ensure all easy handles are removed and cleaned up
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(manager_ctx->multi_handle, &msgs_left))) {
             if (msg->msg == CURLMSG_DONE) {
                // If a request completes during cleanup, it should still be handled.
                // However, the ws_check_multi_info will try to call explorer's callback,
                // which might be problematic if explorer is already being torn down.
                // It's better to explicitly remove and cleanup handles without callbacks during global cleanup.
                CURL *easy_handle = msg->easy_handle;
                ws_http_request_context *ctx;
                curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, (char**)&ctx);
                if (ctx) {
                    // Cleanup associated ws_event_handles if not already done
                    if (ctx->socket_event_read) ws_event_del(ctx->socket_event_read);
                    if (ctx->socket_event_write) ws_event_del(ctx->socket_event_write);
                    curl_slist_free_all(ctx->current_headers_list);
                    zfree(ctx);
                }
                curl_multi_remove_handle(manager_ctx->multi_handle, easy_handle);
                curl_easy_cleanup(easy_handle);
             }
        }
        curl_multi_cleanup(manager_ctx->multi_handle);
    }
    if (manager_ctx->curl_multi_timer_event_handle) { // Cleanup potential remaining timer
        ws_event_del(manager_ctx->curl_multi_timer_event_handle);
        manager_ctx->curl_multi_timer_event_handle = NULL;
    }
    zfree(manager_ctx);
    curl_global_cleanup();
}

bool ws_http_perform_request(ws_http_manager_ctx *manager_ctx,
                             ws_request *request, ws_event_http_cb callback) {
    if (!manager_ctx || !request || !callback) return false;

    CURL *easy_handle = curl_easy_init();
    if (!easy_handle) {
        ws_log_error("curl_easy_init() failed.\n");
        return false;
    }

    ws_http_request_context *ctx = (ws_http_request_context*)zcalloc(sizeof(ws_http_request_context));
    if (!ctx) {
        curl_easy_cleanup(easy_handle);
        ws_log_error("Failed to allocate http request context.\n");
        return false;
    }
    // Initialize the context
    ctx->easy_handle = easy_handle;
    ctx->multi_handle = manager_ctx->multi_handle;
    ctx->manager_ctx = manager_ctx; // Assign the manager context
    ctx->original_request = request;
    ctx->completion_callback = callback;
    ctx->event_base = manager_ctx->event_base;
    ctx->current_headers_list = NULL; // Initialize list to NULL

    request->response = ws_response_new(); // Create response object
    if (!request->response) {
        zfree(ctx);
        curl_easy_cleanup(easy_handle);
        ws_log_error("Failed to create response object.\n");
        return false;
    }

    curl_easy_setopt(easy_handle, CURLOPT_URL, request->url);
    curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, ws_write_data_callback);
    curl_easy_setopt(easy_handle, CURLOPT_WRITEDATA, (void *)request->response);
    curl_easy_setopt(easy_handle, CURLOPT_HEADERFUNCTION, ws_header_callback);
    curl_easy_setopt(easy_handle, CURLOPT_HEADERDATA, (void *)request->response);
    curl_easy_setopt(easy_handle, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects
    curl_easy_setopt(easy_handle, CURLOPT_PRIVATE, ctx); // Set private data for context retrieval

    // SSL/TLS (HTTPS) handling
    if (manager_ctx->event_base->ssl_ctx) {
        curl_easy_setopt(easy_handle, CURLOPT_SSL_CTX_DATA, manager_ctx->event_base->ssl_ctx);
        curl_easy_setopt(easy_handle, CURLOPT_SSL_CTX_FUNCTION, NULL);
        // Default: Verify peer. If you want to disable for testing:
        // curl_easy_setopt(easy_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        // curl_easy_setopt(easy_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        // Fallback for no SSL_CTX, disable verification for demo (NOT RECOMMENDED FOR PROD)
        curl_easy_setopt(easy_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // Cookie handling (libcurl's internal cookie engine)
    // For in-memory cookie jar managed by ws_cookie.c, a custom callback would be needed
    // or manually injecting cookies. Using libcurl's file-based for simplicity.
    curl_easy_setopt(easy_handle, CURLOPT_COOKIEJAR, "cookies.txt"); // Persist cookies to file
    curl_easy_setopt(easy_handle, CURLOPT_COOKIEFILE, "cookies.txt"); // Read cookies from file
    // To handle cookies from ws_cookie_jar, you'd iterate the jar and use CURLOPT_COOKIELIST.
    // This is more involved and not covered in this example.

    curl_mime *mime = NULL;

    // Set method and POST/PUT data
    if (strcmp(request->method, "POST") == 0) {
        if (request->post_is_form_data) {
            // Use modern MIME API for form data, this implies POST
            mime = curl_mime_init(easy_handle);
            curl_mimepart *part;

            // Add form fields
            for (size_t i = 0; i < request->post_data.num_form_params; ++i) {
                part = curl_mime_addpart(mime);
                curl_mime_name(part, request->post_data.form_params[i].key);
                curl_mime_data(part, request->post_data.form_params[i].value, CURL_ZERO_TERMINATED);
            }

            // Add file uploads
            for (size_t i = 0; i < request->num_file_params; ++i) {
                part = curl_mime_addpart(mime);
                curl_mime_name(part, request->file_params[i].field_name);
                curl_mime_filename(part, request->file_params[i].file_name);
                curl_mime_data(part, (const char*)request->file_params[i].file_content, request->file_params[i].file_content_len);
            }
            
            curl_easy_setopt(easy_handle, CURLOPT_MIMEPOST, mime);
        } else if (request->post_data.raw_body) {
            // Standard POST with raw body
            curl_easy_setopt(easy_handle, CURLOPT_POST, 1L);
            curl_easy_setopt(easy_handle, CURLOPT_POSTFIELDS, request->post_data.raw_body);
            curl_easy_setopt(easy_handle, CURLOPT_POSTFIELDSIZE, (long)strlen(request->post_data.raw_body));
        }
    } else if (strcmp(request->method, "PUT") == 0) {
        curl_easy_setopt(easy_handle, CURLOPT_UPLOAD, 1L);
        if (!request->post_is_form_data && request->post_data.raw_body) {
             curl_easy_setopt(easy_handle, CURLOPT_POSTFIELDS, request->post_data.raw_body);
             curl_easy_setopt(easy_handle, CURLOPT_POSTFIELDSIZE, (long)strlen(request->post_data.raw_body));
        } else {
             // For PUT with no raw body, might need a read callback for data source
             curl_easy_setopt(easy_handle, CURLOPT_UPLOAD, 1L);
             curl_easy_setopt(easy_handle, CURLOPT_READDATA, NULL); // Or your read callback
             curl_easy_setopt(easy_handle, CURLOPT_INFILESIZE, 0L); // Or actual size
        }
    } else if (strcmp(request->method, "HEAD") == 0) {
        curl_easy_setopt(easy_handle, CURLOPT_NOBODY, 1L); // Only fetch headers
    }

    // Set custom headers
    struct curl_slist *headers = NULL;
    // Add Content-Type if specified and not a form-data POST (libcurl handles CT for form-data)
    if (request->content_type && strlen(request->content_type) > 0) {
        if (!(strcmp(request->method, "POST") == 0 && request->post_is_form_data)) {
            char ct_header[256];
            snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", request->content_type);
            headers = curl_slist_append(headers, ct_header);
        }
    }
    if (request->referer && strlen(request->referer) > 0) {
        char ref_header[256];
        snprintf(ref_header, sizeof(ref_header), "Referer: %s", request->referer);
        headers = curl_slist_append(headers, ref_header);
    }
    // Add any extra headers from request->extra_headers
    if (request->extra_headers) {
        struct curl_slist *current = request->extra_headers;
        while(current) {
            headers = curl_slist_append(headers, current->data);
            current = current->next;
        }
        // NOTE: We should NOT free request->extra_headers in ws_request_free if we pass it to libcurl
        // here. The curl_slist_append copies the string, but not the list structure itself.
        // The `headers` list is a new list. `request->extra_headers` should be freed by `ws_request_free`.
        // However, if we pass it directly (i.e. `curl_easy_setopt(CURLOPT_HTTPHEADER, request->extra_headers);`),
        // then libcurl owns it.
        // For robustness, we create a new combined list here and free it in the context.
        ctx->current_headers_list = headers; // Store for later cleanup
    }

    if (ctx->current_headers_list) {
        curl_easy_setopt(easy_handle, CURLOPT_HTTPHEADER, ctx->current_headers_list);
    }

    // Add handle to multi
    CURLMcode mc = curl_multi_add_handle(manager_ctx->multi_handle, easy_handle);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_add_handle failed: %s\n", curl_multi_strerror(mc));
        curl_mime_free(mime); // Clean up mime object on failure
        curl_slist_free_all(ctx->current_headers_list); // Clean up headers on failure
        zfree(ctx);
        ws_response_free(request->response); // Free the response here
        request->response = NULL;
        curl_easy_cleanup(easy_handle);
        return false;
    }

    manager_ctx->event_base->total_requests++; // Increment total requests counter

    return true;
}
