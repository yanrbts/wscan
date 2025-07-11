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
#include <stdlib.h>
#include <string.h>
#include <errno.h>      // For errno
#include <sys/types.h>  // For size_t on some systems
#include <sys/socket.h> // For evutil_socket_t (though libevent includes it)
#include <netinet/in.h> // For sockaddr_in (though not directly used, good to have)
#include <arpa/inet.h>  // For inet_ntoa (though not directly used)
#include <fcntl.h>      // For fcntl
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_http.h>

/* Map from fd to socket_events index
 * This is a simple hash map, might need more robust solution for very large FDs
 * or sparse FDs. For now, a direct array access or small hash table is assumed.
 * For large-scale projects, consider a proper hash map for `socket_events`.
 * Adjust based on expected concurrent sockets */
#define MAX_FDS_FOR_HTTP_EVENTS 1024 

/* Internal structure for an individual HTTP request */
struct ws_http_request {
    CURL *easy_handle;                          // Libcurl easy handle for this request
    struct curl_slist *headers;                 // Custom headers for this request
    ws_http_client_t *client;                   // Pointer back to the owning client
    ws_http_header_cb_fn header_cb;
    ws_http_data_cb_fn data_cb;
    ws_http_complete_cb_fn complete_cb;
    void *user_data;
    bool cancelled;                             // Flag to indicate if request was cancelled
};

/* Internal structure for the HTTP client (manages multi-handle and event loop) */
struct ws_http_client {
    CURLM *multi_handle;                        // Libcurl multi handle
    ws_event_loop_t *event_loop;                // Our ws_event_loop_t
    ws_event_t *timer_event;                    // Timer for libcurl's internal timeouts
    struct {
        ws_event_t *event;                      // Pointer to our ws_event_t
        int running_flags;                      // Current event flags being monitored
    } socket_events[MAX_FDS_FOR_HTTP_EVENTS];   // Array to track socket events
                                                // Adjust MAX_FDS_FOR_HTTP_EVENTS as needed.
                                                // 1024 is a common default for typical use.
};

static void s_curl_multi_timer_cb(void *user_data);
static void s_curl_multi_socket_cb(evutil_socket_t fd, short events, void *user_data);
static size_t s_curl_write_data_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
static size_t s_curl_write_header_cb(char *buffer, size_t size, size_t nitems, void *userdata);
static void s_process_curl_messages(ws_http_client_t *client);
static void s_free_http_request(ws_http_request_t *req);
static ws_http_request_t *s_http_request_init(ws_http_client_t *client,
                                            ws_http_header_cb_fn header_cb,
                                            ws_http_data_cb_fn data_cb,
                                            ws_http_complete_cb_fn complete_cb,
                                            void *user_data);

/* This callback is called by Libcurl to tell us when the next timeout should occur.
 * We then arm our ws_event timer. */
static int s_curl_timer_cb(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi;
    ws_http_client_t *client = (ws_http_client_t *)userp;
    /**
     *  if timeout_ms < 0, it means:
     *  - No timeout is pending, or
     *  - Libcurl wants to disable the timer.
     *  In this case, we simply log a warning and do not create a new timer.
     *  If timeout_ms is == 0, Trigger once immediately
     *  if timeout_ms > 0, it means:
     *  - Libcurl wants us to set a timer for the specified milliseconds.
     *  - We will create a new timer event that will call our s_curl_multi_timer_cb
     *    when the timeout expires.
     *  - This is a one-shot timer; it will not repeat unless libcurl calls
     *    this callback again with a new timeout.
     */
    if (timeout_ms >= 0) {
        if (client->timer_event) {
            ws_event_update_timer(client->timer_event, timeout_ms);
        } else {
            /* Arm a new timer
             * Libcurl's timer is always one-shot; it'll call this callback again if needed.*/
            client->timer_event = ws_event_new_timer(client->event_loop, timeout_ms, false,
                                                    s_curl_multi_timer_cb, client);
            
            if (!client->timer_event) {
                ws_log_error("Failed to create libcurl multi timer event.");
                return -1;
            }

            if (!ws_event_add(client->timer_event)) {
                ws_log_error("Failed to add libcurl multi timer event.");
                ws_event_free(client->timer_event);
                client->timer_event = NULL;
                return -1;
            }
        }
    } else {
        /* timeout_ms == -1 means no timeout is pending, 
         * or it wants to disable the timer. */
        if (client->timer_event) {
            ws_event_del(client->timer_event);
        }
    }

    return 0;
}

/* Our ws_event timer callback, called when libcurl's timeout expires */
static void s_curl_multi_timer_cb(void *user_data) {
    ws_http_client_t *client = (ws_http_client_t *)user_data;

    if (!client || !client->multi_handle) {
        ws_log_error("Invalid client in s_curl_multi_timer_cb.");
        return;
    }

    // ws_log_debug("Libcurl multi timer fired. Checking for activity...");

    int still_running;
    CURLMcode mc = curl_multi_socket_action(client->multi_handle, CURL_SOCKET_TIMEOUT, 0, &still_running);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_socket_action (timeout) failed: %s", curl_multi_strerror(mc));
    }

    s_process_curl_messages(client);
}

/* This callback is called by Libcurl to tell us what socket to watch and for what events.
 * We then add/remove/modify our ws_event I/O events. */
static int s_curl_socket_cb(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp) {
    (void)easy;
    (void)socketp;
    ws_http_client_t *client = (ws_http_client_t *)userp;

    if (s < 0 || s >= MAX_FDS_FOR_HTTP_EVENTS) {
        ws_log_error("Socket FD %d out of bounds for MAX_FDS_FOR_HTTP_EVENTS (%d). Increase MAX_FDS_FOR_HTTP_EVENTS.", 
                    (int)s, MAX_FDS_FOR_HTTP_EVENTS);
        return -1;
    }

    switch (what) {
        case CURL_POLL_NONE:
            /* Curl doesn't want to monitor this socket anymore. */
            if (client->socket_events[s].event) {
                ws_event_del(client->socket_events[s].event);
                ws_event_free(client->socket_events[s].event);
                client->socket_events[s].event = NULL;
            }
            client->socket_events[s].running_flags = 0;
            break;
        case CURL_POLL_IN:
        case CURL_POLL_OUT:
        case CURL_POLL_INOUT:
            {
                // Curl wants to monitor this socket for read/write.
                short new_ev_flags = 0;

                if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
                    new_ev_flags |= WS_EV_READ;
                }

                if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
                    new_ev_flags |= WS_EV_WRITE;
                }
                // Always persistent for multi_socket_action
                new_ev_flags |= WS_EV_PERSIST; 

                // If an event already exists for this socket, remove it first
                if (client->socket_events[s].event) {
                    ws_event_del(client->socket_events[s].event);
                    ws_event_free(client->socket_events[s].event);
                    client->socket_events[s].event = NULL;
                }

                // Create a new event or re-create with updated flags
                // Pass the client pointer as user_data for the socket callback
                client->socket_events[s].event = ws_event_new_io(client->event_loop, s, new_ev_flags,
                                                                s_curl_multi_socket_cb, client);
                if (!client->socket_events[s].event) {
                    ws_log_error("Failed to create ws_event for curl socket %d (flags %d).", 
                                (int)s, (int)new_ev_flags);
                    return -1;
                }

                if (!ws_event_add(client->socket_events[s].event)) {
                    ws_log_error("Failed to add ws_event for curl socket %d (flags %d).", 
                                (int)s, (int)new_ev_flags);
                    ws_event_free(client->socket_events[s].event);
                    client->socket_events[s].event = NULL;
                    return -1;
                }
                client->socket_events[s].running_flags = new_ev_flags;
                break;
            }
        case CURL_POLL_REMOVE:
            // Curl wants to remove this socket from monitoring.
            if (client->socket_events[s].event) {
                ws_event_del(client->socket_events[s].event);
                ws_event_free(client->socket_events[s].event);
                client->socket_events[s].event = NULL;
            }
            client->socket_events[s].running_flags = 0;
            break;
        default:
            ws_log_warn("Unknown 'what' value received from curl_multi_assign: %d", what);
            break;
    }
    return 0;
}

/* Our ws_event I/O callback, called when socket activity occurs */
static void s_curl_multi_socket_cb(evutil_socket_t fd, short events, void *user_data) {
    ws_http_client_t *client = (ws_http_client_t *)user_data;
    if (!client || !client->multi_handle) {
        ws_log_error("Invalid client in s_curl_multi_socket_cb.");
        return;
    }

    int curl_action_flags = 0;
    if (events & WS_EV_READ) {
        curl_action_flags |= CURL_CSELECT_IN;
    }

    if (events & WS_EV_WRITE) {
        curl_action_flags |= CURL_CSELECT_OUT;
    }

    // Libevent might also report exceptions, which map to CURL_CSELECT_ERR
    // (e.g., if a socket closes or has an error)
    if (events & EV_READ && events & EV_WRITE) { // A crude way to check for 'both'
        // Libevent's EV_READ/EV_WRITE might also implicitly cover errors.
        // For more robust error handling, consider EV_CLOSED and other specific events.
    }

    int still_running;
    CURLMcode mc = curl_multi_socket_action(client->multi_handle, fd, curl_action_flags, &still_running);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_socket_action (socket action) failed: %s", curl_multi_strerror(mc));
    }

    s_process_curl_messages(client);
}

/* Callback for writing response body data */
static size_t s_curl_write_data_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ws_http_request_t *req = (ws_http_request_t *)userdata;
    if (req && req->data_cb && !req->cancelled) {
        req->data_cb(ptr, size * nmemb, req->user_data);
    }
    return size * nmemb;
}

/* Callback for writing response header data */
static size_t s_curl_write_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    ws_http_request_t *req = (ws_http_request_t *)userdata;
    if (req && req->header_cb && !req->cancelled) {
        req->header_cb(buffer, req->user_data);
    }
    return size * nitems;
}

/* Checks completed transfers and invokes user callbacks */
static void s_process_curl_messages(ws_http_client_t *client) {
    CURLMsg *msg;
    int msgs_left;
    CURL *easy_handle;
    long http_code = 0;
    CURLcode result;
    ws_http_request_t *req = NULL;

    while ((msg = curl_multi_info_read(client->multi_handle, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            easy_handle = msg->easy_handle;
            result = msg->data.result;

            // Get our custom request object back from the easy handle
            curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &req);

            if (req && !req->cancelled) {
                // Get HTTP response code
                curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &http_code);

                if (req->complete_cb) {
                    req->complete_cb(req, http_code, result, req->user_data);
                }
            } else {
                ws_log_info("Completed request %p was cancelled or invalid. Not invoking callback.", (void*)req);
            }

            // Remove the easy handle from the multi handle
            curl_multi_remove_handle(client->multi_handle, easy_handle);
            // Free the easy handle and our request object
            s_free_http_request(req);
        }
    }
}

// Frees a single ws_http_request_t instance and its associated curl handle
static void s_free_http_request(ws_http_request_t *req) {
    if (!req) return;

    if (req->easy_handle) {
        curl_easy_cleanup(req->easy_handle);
        req->easy_handle = NULL;
    }
    if (req->headers) {
        curl_slist_free_all(req->headers);
        req->headers = NULL;
    }
    zfree(req);
}

// Common initialization for a new HTTP request
static ws_http_request_t *s_http_request_init(ws_http_client_t *client,
                                            ws_http_header_cb_fn header_cb,
                                            ws_http_data_cb_fn data_cb,
                                            ws_http_complete_cb_fn complete_cb,
                                            void *user_data) {
    if (!client || !client->multi_handle || !complete_cb) {
        ws_log_error("Invalid client or NULL complete callback for new request.");
        return NULL;
    }

    ws_http_request_t *req = zcalloc(sizeof(ws_http_request_t));
    if (!req) {
        ws_log_error("Failed to allocate memory for ws_http_request_t.");
        return NULL;
    }

    req->easy_handle = curl_easy_init();
    if (!req->easy_handle) {
        ws_log_error("Failed to create CURL easy handle.");
        zfree(req);
        return NULL;
    }

    req->client = client;
    req->header_cb = header_cb;
    req->data_cb = data_cb;
    req->complete_cb = complete_cb;
    req->user_data = user_data;
    req->cancelled = false;

    // Set common curl options
    curl_easy_setopt(req->easy_handle, CURLOPT_PRIVATE, req);                               // Store our request object
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, s_curl_write_data_cb);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, req);                             // Pass our request object
    curl_easy_setopt(req->easy_handle, CURLOPT_HEADERFUNCTION, s_curl_write_header_cb);
    curl_easy_setopt(req->easy_handle, CURLOPT_HEADERDATA, req);                            // Pass our request object
    curl_easy_setopt(req->easy_handle, CURLOPT_NOSIGNAL, 1L);                               // Crucial for multi-threaded apps
    curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);                         // Follow redirects
    curl_easy_setopt(req->easy_handle, CURLOPT_VERBOSE, 0L);                                // Set to 1L for libcurl debug info

    return req;
}

ws_http_client_t *ws_http_client_new(ws_event_loop_t *loop) {
    if (!loop) {
        ws_log_error("Cannot create ws_http_client with a NULL event loop.");
        return NULL;
    }

    ws_http_client_t *client = zcalloc(sizeof(ws_http_client_t));
    if (!client) {
        ws_log_error("Failed to allocate memory for ws_http_client_t.");
        return NULL;
    }

    // Initialize libcurl global state (only once per process)
    CURLcode curl_global_init_res = curl_global_init(CURL_GLOBAL_ALL);
    if (curl_global_init_res != CURLE_OK) {
        ws_log_error("curl_global_init failed: %s", curl_easy_strerror(curl_global_init_res));
        zfree(client);
        return NULL;
    }

    client->multi_handle = curl_multi_init();
    if (!client->multi_handle) {
        ws_log_error("Failed to create CURL multi handle.");
        zfree(client);
        /* Note: curl_global_cleanup() should be called, but it's hard to track
         * if this is the only client or not. Typically done at app shutdown. */
        return NULL;
    }

    client->event_loop = loop;

    // Set libcurl callbacks for socket and timer management
    curl_multi_setopt(client->multi_handle, CURLMOPT_SOCKETFUNCTION, s_curl_socket_cb);
    curl_multi_setopt(client->multi_handle, CURLMOPT_SOCKETDATA, client);
    curl_multi_setopt(client->multi_handle, CURLMOPT_TIMERFUNCTION, s_curl_timer_cb);
    curl_multi_setopt(client->multi_handle, CURLMOPT_TIMERDATA, client);

    // Initialize socket event tracking array
    for (int i = 0; i < MAX_FDS_FOR_HTTP_EVENTS; ++i) {
        client->socket_events[i].event = NULL;
        client->socket_events[i].running_flags = 0;
    }

    ws_log_info("HTTP client created and initialized.");
    return client;
}

void ws_http_client_free(ws_http_client_t *client) {
    if (!client) return;

    // Remove any pending requests from the multi handle and free them
    CURLMsg *msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(client->multi_handle, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            ws_http_request_t *req;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req);
            if (req) {
                ws_log_debug("Freeing pending request %p during client shutdown.", (void*)req);
                curl_multi_remove_handle(client->multi_handle, msg->easy_handle);
                s_free_http_request(req);
            }
        }
    }

    // Clean up any remaining socket events
    for (int i = 0; i < MAX_FDS_FOR_HTTP_EVENTS; ++i) {
        if (client->socket_events[i].event) {
            ws_event_del(client->socket_events[i].event);
            ws_event_free(client->socket_events[i].event);
            client->socket_events[i].event = NULL;
        }
    }

    // Clean up timer event
    if (client->timer_event) {
        ws_event_del(client->timer_event);
        ws_event_free(client->timer_event);
        client->timer_event = NULL;
    }

    // Clean up curl multi handle
    if (client->multi_handle) {
        curl_multi_cleanup(client->multi_handle);
        client->multi_handle = NULL;
    }

    zfree(client);
    ws_log_info("HTTP client freed.");

    // Note: curl_global_cleanup() is usually called once at application exit.
    // It's not safe to call it here unless you are absolutely sure this is
    // the last HTTP client instance and no other part of the app uses libcurl.
}

static ws_http_request_t *s_http_perform_request(ws_http_client_t *client, const char *url,
                                                const char *post_data, size_t post_data_len,
                                                ws_http_header_cb_fn header_cb,
                                                ws_http_data_cb_fn data_cb,
                                                ws_http_complete_cb_fn complete_cb,
                                                void *user_data, bool is_post) {
    ws_http_request_t *req = s_http_request_init(client, header_cb, data_cb, complete_cb, user_data);
    if (!req) return NULL;

    curl_easy_setopt(req->easy_handle, CURLOPT_URL, url);

    if (is_post) {
        curl_easy_setopt(req->easy_handle, CURLOPT_POST, 1L);
        curl_easy_setopt(req->easy_handle, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(req->easy_handle, CURLOPT_POSTFIELDSIZE, (long)post_data_len);
    }

    CURLMcode mc = curl_multi_add_handle(client->multi_handle, req->easy_handle);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_add_handle failed for URL %s: %s", url, curl_multi_strerror(mc));
        s_free_http_request(req);
        return NULL;
    }

    // Initiate polling for the first time by performing a socket_action with -1
    int still_running;
    mc = curl_multi_socket_action(client->multi_handle, CURL_SOCKET_TIMEOUT, 0, &still_running);
    if (mc != CURLM_OK) {
        ws_log_error("Initial curl_multi_socket_action failed for URL %s: %s", url, curl_multi_strerror(mc));
        curl_multi_remove_handle(client->multi_handle, req->easy_handle);
        s_free_http_request(req);
        return NULL;
    }
    s_process_curl_messages(client); // Check for immediate completions (e.g., cached)

    // ws_log_info("HTTP %s request for URL '%s' started.", is_post ? "POST" : "GET", url);
    return req;
}

ws_http_request_t *ws_http_get(ws_http_client_t *client, const char *url,
                               ws_http_header_cb_fn header_cb,
                               ws_http_data_cb_fn data_cb,
                               ws_http_complete_cb_fn complete_cb,
                               void *user_data) {
    return s_http_perform_request(client, url, NULL, 0, header_cb, data_cb, complete_cb, user_data, false);
}

ws_http_request_t *ws_http_post(ws_http_client_t *client, const char *url,
                                const char *post_data, size_t post_data_len,
                                ws_http_header_cb_fn header_cb,
                                ws_http_data_cb_fn data_cb,
                                ws_http_complete_cb_fn complete_cb,
                                void *user_data) {
    if (!post_data) {
        ws_log_error("POST request requires post_data.");
        return NULL;
    }
    return s_http_perform_request(client, url, post_data, post_data_len, header_cb, 
                                data_cb, complete_cb, user_data, true);
}


bool ws_http_cancel_request(ws_http_client_t *client, ws_http_request_t *request) {
    if (!client || !request) {
        ws_log_warn("Attempted to cancel NULL client or request.");
        return false;
    }

    if (request->cancelled) {
        ws_log_info("Request %p already marked as cancelled.", (void*)request);
        return true;
    }

    request->cancelled = true; // Mark as cancelled so callbacks don't fire
    ws_log_info("Attempting to cancel request %p.", (void*)request);

    // Remove from multi-handle. This will cause the easy handle to be cleaned up
    // by libcurl's internal message processing if it completes later, or we clean it up here.
    CURLMcode mc = curl_multi_remove_handle(client->multi_handle, request->easy_handle);
    if (mc != CURLM_OK) {
        ws_log_error("Failed to remove easy handle %p from multi handle: %s", (void*)request->easy_handle, curl_multi_strerror(mc));
        return false;
    }
    
    // Explicitly free the request object immediately after removal
    s_free_http_request(request);
    ws_log_info("Request %p successfully cancelled and freed.", (void*)request);

    // Force a socket_action to allow libcurl to re-evaluate its state
    // after a handle has been removed. This might not be strictly necessary
    // in all cases but helps ensure multi-handle state is up-to-date.
    int still_running;
    mc = curl_multi_socket_action(client->multi_handle, CURL_SOCKET_TIMEOUT, 0, &still_running);
    if (mc != CURLM_OK) {
        ws_log_error("curl_multi_socket_action after cancel failed: %s", curl_multi_strerror(mc));
    }
    s_process_curl_messages(client); // Check for any immediately completed messages

    return true;
}
