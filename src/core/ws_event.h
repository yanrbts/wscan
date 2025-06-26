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
#include <event2/keyvalq_struct.h> // For struct evkeyvalq
#include <ws_rbtree.h>
#include <ws_ssl.h>

/**
 * @brief HTTP request completion callback function type.
 * @param status_code HTTP status code (e.g., 200, 404). 0 for timeout, -1 for other errors.
 * @param headers HTTP response headers.
 * @param body Response body data.
 * @param body_len Length of the response body.
 * @param user_data User-provided argument.
 * @param error_code 0 for success, non-zero for errors (e.g., -1 for timeout, -2 for connection error).
 */
typedef void (*ws_event_http_cb)(int status_code, struct evkeyvalq *headers,
                                 const char *body, size_t body_len,
                                 void *user_data, int error_code);

// Generic event callback function type
typedef void (*ws_event_cb)(int fd, short events, void *arg);

// Event type enum
typedef enum {
    WS_EVENT_NORMAL = 0,
    WS_EVENT_HTTP = 1,
    WS_EVENT_TIME = 2
} ws_event_type;

// Event base structure
typedef struct ws_event_base {
    struct event_base *base;
    long long next_event_id; // Used to generate unique event IDs
    int total_requests;
    int success_requests;
    int failed_requests;
    rbTable *events; // Red-black tree to manage events
    SSL_CTX *ssl_ctx;
} ws_event_base;

// Event handle structure
typedef struct ws_event_handle {
    long long id;              // Unique identifier, used as the key for the red-black tree
    ws_event_type type;        // Event type (Normal, HTTP, Time)
    ws_event_base *base;       // Pointer to its owning event base

    union {
        /* Normal event */
        struct {
            int fd;
            struct event *ev;
            ws_event_cb callback;
            bool is_persistent;
        } normal;
        /* HTTP event */
        struct {
            // Note: http.conn is no longer explicitly saved in ws_event_handle
            // Its lifecycle is managed by libevent, or by a connection pool if needed
            ws_event_http_cb callback;
            void *internal_ctx; // Pointer to the internal HTTP request context
        } http;
        /* Time event */
        struct {
            struct event *ev;
            ws_event_cb callback;
            struct timeval timeout;
            bool is_persistent;
        } time;
    };

    void *arg;
} ws_event_handle;

/**
 * @brief Creates a new event base.
 * This function initializes a new event base, which is the core of the event loop.
 * It allocates memory for the ws_event_base structure and sets up the event base.
 * @return A pointer to the newly created ws_event_base structure, or NULL on failure.
 */
ws_event_base *ws_event_new(void);

/**
 * @brief Frees the ws_event_base structure and its associated resources.
 * This function is responsible for cleaning up the event base and freeing memory
 * allocated for the ws_event_base structure. It ensures that all resources
 * are properly released to prevent memory leaks.
 * @param we A pointer to the ws_event_base structure to be freed.
 */
void ws_event_free(ws_event_base *we);

/**
 * @brief Deletes an event handle.
 * This function is responsible for removing an event from the event loop and
 * freeing associated resources. It ensures that the event is no longer monitored
 * and memory is properly released.
 * @param handle A pointer to the ws_event_handle structure to be deleted.
 */
void ws_event_del(ws_event_handle *handle);

/**
 * @brief Adds a normal event to the event loop.
 * This function creates a new event that will trigger when a file descriptor
 * is ready for reading or writing. It allows for asynchronous I/O operations,
 * useful for network sockets or file descriptors.
 * @param we A pointer to the ws_event_base structure.
 * @param fd The file descriptor to monitor.
 * @param events The events to monitor (e.g., EV_READ, EV_WRITE).
 * @param callback The callback function to call when the event triggers.
 * @param arg An argument to pass to the callback function.
 * @param is_persistent If true, the event will be persistent and will
 * repeatedly trigger until explicitly removed.
 * @return A pointer to the newly created ws_event_handle structure, or NULL on failure.
 */
ws_event_handle *ws_event_add(ws_event_base *we,
    int fd, short events,
    ws_event_cb callback, void *arg, bool is_persistent);

/**
 * @brief Adds a time-based event to the event loop.
 * This function creates a new time-based event that will trigger after a
 * specified timeout duration. It allows for scheduling events to occur after
 * a certain period, useful for tasks like timeouts or periodic checks.
 * @param we A pointer to the ws_event_base structure.
 * @param tv A pointer to a timeval structure specifying the timeout duration.
 * @param callback The callback function to call when the event triggers.
 * @param arg An argument to pass to the callback function.
 * @param is_persistent If true, the event will be persistent and will
 * repeatedly trigger until explicitly removed.
 * @return A pointer to the newly created ws_event_handle structure, or NULL on failure.
 */
ws_event_handle *ws_event_add_time(ws_event_base *we,
    const struct timeval *tv, ws_event_cb callback, void *arg, bool is_persistent);

/**
 * @brief Starts the event loop.
 * This function starts the event loop, which will run until there are no
 * more events to process or the loop is stopped. It processes events and
 * calls associated callbacks as events occur.
 * @param we A pointer to the ws_event_base structure.
 * @return 0 on successful loop exit, or -1 if an error occurred.
 */
int ws_event_loop(ws_event_base *we);

/**
 * @brief Stops the event loop.
 * This function stops the event loop, allowing it to exit gracefully.
 * It is typically called during application shutdown or when event processing
 * is no longer needed.
 * @param we A pointer to the ws_event_base structure.
 */
void ws_event_stop(ws_event_base *we);

/**
 * @brief Inserts an event handle into the event base's red-black tree.
 * This function is now public for use by other modules (e.g., ws_http.c).
 * @param we The event base.
 * @param item The handle to insert.
 * @return The inserted handle, or NULL on error (e.g., duplicate ID).
 */
ws_event_handle *ws_insert_event(ws_event_base *we, ws_event_handle *item);

#endif // __WS_EVENT_H__