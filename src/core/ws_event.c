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
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_event.h>

static void ws_event_internal_cb(evutil_socket_t fd, short events, void *arg) {
    ws_event_handle *handle = (ws_event_handle *)arg;
    if (handle && handle->normal.callback) {
        /* Call the user-defined callback with the file descriptor and events */
        handle->normal.callback(fd, events, handle->arg);
    }
}

static void ws_event_internal_http_cb(struct evhttp_request *req, void *arg) {
    ws_event_handle *handle = (ws_event_handle *)arg;
    if (handle && handle->http.callback) {

        struct evbuffer *inbuf = evhttp_request_get_input_buffer(req);
        if (!inbuf) {
            /* If the input buffer is NULL, we cannot process the request */
            ws_log_error("HTTP request input buffer is NULL");

            // handle->http.callback(req, handle->arg);
            if (handle->http.conn) {
                /* Free the HTTP connection if it exists */
                evhttp_connection_free(handle->http.conn);
            }
        }

        zfree(handle);
        /* Call the user-defined callback with the HTTP request */
        // handle->http.callback(req, handle->arg);
        /* Free the HTTP request */
        // evhttp_request_free(req);
    }
}

/**
 * Create a new ws_base structure.
 * This function initializes a new ws_base structure, which is used to manage events in the event loop.
 * It allocates memory for the structure and initializes the event base and event statistics.
 *
 * @return A pointer to the newly created ws_base structure, or NULL if memory allocation fails.
 */
ws_event_base *ws_event_new(void) {
    ws_event_base *we = zmalloc(sizeof(ws_event_base));
    if (!we) {
        return NULL;
    }
    /* Initialize the event base
     * The event base is the core of the event loop, and it is responsible for managing */
    we->base = event_base_new();
    if (!we->base) {
        zfree(we);
        return NULL;
    }
    /* Initialize event statistics
     * These values are set to zero to ensure that the event statistics are initialized correctly.
     * This is important for accurate tracking of event performance and debugging. */
    we->total_requests = 0;
    we->success_requests = 0;
    we->failed_requests = 0;

    return we;
}

/**
 * Free the ws_event structure and its associated resources.
 * This function is responsible for cleaning up the event base and freeing the memory 
 * allocated for the ws_event structure.
 * It ensures that all resources are properly released to prevent memory leaks.
 *
 * @param we Pointer to the ws_event structure to be freed.
 */
void ws_event_free(ws_event_base *we) {
    if (we) {
        /* Free the event base
         * This is important to release resources associated with the event loop. */
        if (we->base) {
            event_base_free(we->base);
        }
        /* Free the ws_event structure itself */
        zfree(we);
    }
}

void ws_event_del(ws_event_handle *handle) {
    if (handle) {
        /* Free the event associated with the handle */
        if (handle->type == WS_EVENT_NORMAL && handle->normal.ev) {
            event_del(handle->normal.ev);
            event_free(handle->normal.ev);
        } else if (handle->type == WS_EVENT_HTTP && handle->http.conn) {
            /* Free the HTTP connection */
            evhttp_connection_free(handle->http.conn);
        }
        /* Free the handle itself */
        zfree(handle);
    }
}

ws_event_handle *ws_event_add(ws_event_base *we, int fd, 
    short events, ws_event_cb callback, void *arg, bool is_persistent)
{
    short flags;
    /* Invalid parameters */
    if (!we || !we->base || !callback) {
        return NULL; 
    }

    /* Allocate memory for the event handle */
    ws_event_handle *handle = zmalloc(sizeof(ws_event_handle));
    if (!handle) {
        return NULL; 
    }

    /* Set the callback and argument*/
    handle->type = WS_EVENT_NORMAL; 
    handle->normal.callback = callback;
    handle->arg = arg;
    handle->normal.is_persistent = is_persistent;

    flags = events;
    if (is_persistent) flags |= EV_PERSIST; // Set the event to be persistent if required

    /* Create a new event */
    handle->normal.ev = event_new(we->base, fd, flags, ws_event_internal_cb, handle);
    if (!handle->normal.ev) {
        zfree(handle);
        return NULL; 
    }
    /* Add the event to the event base */
    if (event_add(handle->normal.ev, NULL) < 0) {
        event_free(handle->normal.ev);
        zfree(handle);
        return NULL; 
    }

    return handle;
}

/* This function is a placeholder for adding HTTP events.
 * In a real implementation, it would create an HTTP request and add it to the event
 * loop. For now, it simply returns NULL to indicate that this functionality is not implemented. */
ws_event_handle *ws_event_add_http(ws_event_base *we, 
    const char *host, int port, const char *uri, 
    ws_event_http_cb callback, void *arg)
{
    struct evhttp_connection *conn;
    struct evhttp_request *req;

    /* Check if the event base is valid */
    if (!we || !we->base || !host || !uri || !callback) {
        return NULL;
    }

    /* Allocate memory for the event handle */
    ws_event_handle *handle = zmalloc(sizeof(ws_event_handle));
    if (!handle) {
        return NULL;
    }

    /* Set the callback and argument */
    handle->type = WS_EVENT_HTTP;
    handle->http.callback = callback;
    handle->arg = arg;

    conn = evhttp_connection_base_new(we->base, NULL, host, port);
    if (!conn) {
        zfree(handle);
        return NULL; 
    }
    handle->http.conn = conn;

    req = evhttp_request_new(ws_event_internal_http_cb, handle);
    if (!req) {
        evhttp_connection_free(conn);
        zfree(handle);
        return NULL;
    }
    evhttp_make_request(conn, req, EVHTTP_REQ_GET, uri);
    return handle;
}

int ws_event_loop(ws_event_base *we) {
    /* Check if the event base is valid */
    if (!we || !we->base) 
        return -1; 
    /* Start the event loop */
    if (event_base_dispatch(we->base) < 0)
        return -1; 
    return 0; 
}

void ws_event_stop(ws_event_base *we) {
    /* Check if the event base is valid */
    if (!we || !we->base) 
        return;
    /* Stop the event loop */
    event_base_loopbreak(we->base);
}