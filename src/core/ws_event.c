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
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_event.h>
#include <ws_util.h>

static int ws_rb_comparison_func(const void *rb_a, const void *rb_b, void *rb_param) {
    UNUSED(rb_param);
    const ws_event_handle *a = (const ws_event_handle *)rb_a;
    const ws_event_handle *b = (const ws_event_handle *)rb_b;
    return (a->id < b->id) ? -1 : (a->id > b->id) ? 1 : 0;
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

// Internal function: Inserts an event into the red-black tree
ws_event_handle *ws_insert_event(ws_event_base *we, ws_event_handle *item) {
    void **cp = rbProbe(we->events, item);
    if (!cp) {
        ws_log_error("Failed to insert event handle %lld into red-black tree.", item->id);
        return NULL;
    }
    if (*cp != item) {
        // Theoretically, this should not happen when using unique IDs as keys.
        // If it occurs, it means the generated ID is duplicated, or the tree's comparison function has issues.
        ws_log_error("Duplicate event ID %lld found in tree. Original: %p, New: %p", item->id, *cp, item);
        return NULL;
    }
    return item;
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