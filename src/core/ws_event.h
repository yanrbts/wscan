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
#include <event2/util.h>
#include <event2/event.h>

/* Forward declarations to keep internal structure opaque */
typedef struct ws_event_loop    ws_event_loop_t;
typedef struct ws_event         ws_event_t;

/**
 * @brief Callback for I/O (socket) events.
 * @param fd The file descriptor (socket).
 * @param events The events that occurred (e.g., WS_EV_READ, WS_EV_WRITE).
 * @param user_data Custom data passed by the user when creating the event.
 */
typedef void (*ws_io_callback_fn)(evutil_socket_t fd, short events, void *user_data);

/**
 * @brief Callback for timer events.
 * @param user_data Custom data passed by the user when creating the event.
 */
typedef void (*ws_timer_callback_fn)(void *user_data);

#define WS_EV_TIMEOUT    EV_TIMEOUT  // Timer event
#define WS_EV_READ       EV_READ     // Read readiness notification
#define WS_EV_WRITE      EV_WRITE    // Write readiness notification
#define WS_EV_PERSIST    EV_PERSIST  // Make event persistent (auto-readd after triggering)
#define WS_EV_ET         EV_ET       // Edge-triggered (advanced I/O)

/**
 * @brief Creates a new event loop instance.
 * @return A pointer to the new event loop, or NULL on failure.
 */
ws_event_loop_t *ws_event_loop_new(void);

/**
 * @brief Frees an event loop and all associated events.
 * Does NOT free user_data passed with events; caller is responsible for that.
 * @param loop The event loop to free.
 */
void ws_event_loop_free(ws_event_loop_t *loop);

/**
 * @brief Starts the event loop dispatching. This function blocks until the loop
 * is stopped or no more events are registered.
 * @param loop The event loop to dispatch.
 * @return true if dispatch completed successfully, false on error.
 */
bool ws_event_loop_dispatch(ws_event_loop_t *loop);

/**
 * @brief Stops the event loop from dispatching. This will cause ws_event_loop_dispatch
 * to return. Can be called from any thread.
 * @param loop The event loop to stop.
 */
void ws_event_loop_stop(ws_event_loop_t *loop);

// --- Event Creation and Management ---

/**
 * @brief Creates a new I/O (socket) event.
 * The event will monitor the given file descriptor for read/write readiness.
 * @param loop The event loop to associate the event with.
 * @param fd The file descriptor (socket) to monitor.
 * @param flags Combinations of WS_EV_READ, WS_EV_WRITE, WS_EV_PERSIST, WS_EV_ET.
 * @param callback The function to call when the event triggers.
 * @param user_data Custom data to pass to the callback. This module does not manage
 * the lifetime of user_data.
 * @return A pointer to the new event, or NULL on failure.
 */
ws_event_t *ws_event_new_io(ws_event_loop_t *loop, evutil_socket_t fd, short flags,
                            ws_io_callback_fn callback, void *user_data);

/**
 * @brief Creates a new timer event.
 * @param loop The event loop to associate the event with.
 * @param timeout_ms The timeout duration in milliseconds.
 * @param is_persistent If true, the timer will re-arm automatically after triggering.
 * @param callback The function to call when the timer triggers.
 * @param user_data Custom data to pass to the callback. This module does not manage
 * the lifetime of user_data.
 * @return A pointer to the new event, or NULL on failure.
 */
ws_event_t *ws_event_new_timer(ws_event_loop_t *loop, long timeout_ms, bool is_persistent,
                               ws_timer_callback_fn callback, void *user_data);

/** 
 * @brief Updates the timeout of an existing timer event.
 * This can be used to change the timeout dynamically.
 * @param event The timer event to update.
 * @param new_timeout_ms The new timeout duration in milliseconds.
 * @return true on success, false if the event is not a timer or on error.
 */
bool ws_event_update_timer(ws_event_t *event, long new_timeout_ms);

/**
 * @brief Adds an event to the event loop, making it active.
 * @param event The event to add.
 * @return true on success, false on failure.
 */
bool ws_event_add(ws_event_t *event);

/**
 * @brief Removes an event from the event loop, making it inactive.
 * Does not free the event structure itself.
 * @param event The event to remove.
 * @return true on success, false if the event was not active or on error.
 */
bool ws_event_del(ws_event_t *event);

/**
 * @brief Frees an event structure.
 * Must be called only after the event has been removed from the loop (if active).
 * Does NOT free user_data.
 * @param event The event to free.
 */
void ws_event_free(ws_event_t *event);

#endif