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
#include <stdio.h>
#include <stdlib.h> 
#include <string.h>
#include <ws_log.h>
#include <ws_malloc.h>
#include <ws_event.h>

struct ws_event_loop {
    struct event_base *base;
    bool stop_flag; // Flag for graceful shutdown
};

/* Represents our generic event wrapper
 * This union allows us to store different callback types efficiently */
struct ws_event {
    struct event *ev;             // The underlying Libevent event
    ws_event_loop_t *loop;        // Pointer back to the owning loop

    // Callback and user data
    union {
        ws_io_callback_fn io_cb;
        ws_timer_callback_fn timer_cb;
    } cb;
    void *user_data;

    short evtype_flags;            // Store original flags for proper callback dispatch
    long timeout_ms_val;           // Stores the timeout for timer events (ADDED FOR ROBUST TIMERS)
};

/* Generic callback for I/O events from Libevent */
static void s_event_io_cb(evutil_socket_t fd, short events, void *arg) {
    ws_event_t *ev = (ws_event_t *)arg;

    if (ev && (ev->evtype_flags & (WS_EV_READ | WS_EV_WRITE | WS_EV_ET))) {
        if (ev->cb.io_cb)
            ev->cb.io_cb(fd, events, ev->user_data);
        else
            ws_log_warn("ws_event_t (%p) triggered IO but has no io_cb defined.", (void*)ev);
    } else {
        ws_log_error("Invalid or unexpected event type in s_event_io_cb for event %p (fd %d, events %d).", 
                    (void*)ev, fd, events);
    }
}

/* Generic callback for timer events from Libevent */
static void s_event_timer_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;

    ws_event_t *ev = (ws_event_t *)arg;

    if (ev && (ev->evtype_flags & WS_EV_TIMEOUT)) {
        if (ev->cb.timer_cb)
            ev->cb.timer_cb(ev->user_data);
        else
            ws_log_warn("ws_event_t (%p) triggered Timer but has no timer_cb defined.", (void*)ev);
    } else {
        ws_log_error("Invalid or unexpected event type in s_event_timer_cb for event %p.", (void*)ev);
    }
}

ws_event_loop_t *ws_event_loop_new(void) {
    ws_event_loop_t *loop = zcalloc(sizeof(ws_event_loop_t));
    if (!loop) {
        ws_log_error("Failed to allocate memory for ws_event_loop_t.");
        return NULL;
    }

    loop->base = event_base_new();
    if (!loop->base) {
        ws_log_error("Failed to create libevent event_base.");
        zfree(loop);
        return NULL;
    }
    ws_log_info("Libevent loop created.");
    return loop;
}

void ws_event_loop_free(ws_event_loop_t *loop) {
    if (!loop) return;

    /* It's crucial that all events associated with this base are removed/freed
     * before freeing the base itself. Libevent typically handles this
     * if you event_free() each event, but it's good practice to ensure. */
    if (loop->base) {
        event_base_free(loop->base);
        ws_log_info("Libevent event_base freed.");
    }
    zfree(loop);
    ws_log_info("Event loop freed.");
}

bool ws_event_loop_dispatch(ws_event_loop_t *loop) {
    if (!loop || !loop->base) {
        ws_log_error("Cannot dispatch on a NULL or uninitialized event loop.");
        return false;
    }

    ws_log_info("Starting event loop dispatch.");
    loop->stop_flag = false; // Reset stop flag before dispatching

    /* event_base_dispatch returns:
     *  0: no events were active, or event_base_loopbreak() was called.
     *  1: error
     * -1: internal error */
    int result = event_base_dispatch(loop->base);

    if (result == -1) {
        ws_log_error("Error occurred during event loop dispatch.");
        return false;
    } else if (result == 1) {
        ws_log_info("Event loop stopped (no events or loopbreak called).");
    } else {
        ws_log_info("Event loop dispatch completed normally.");
    }

    return true;
}

void ws_event_loop_stop(ws_event_loop_t *loop) {
    if (!loop || !loop->base) return;
    
    loop->stop_flag = true;
    /* event_base_loopbreak() is thread-safe 
     * and causes event_base_dispatch() to return */
    event_base_loopbreak(loop->base);
    ws_log_info("Event loop stop requested.");
}

static ws_event_t *s_ws_event_new_common(ws_event_loop_t *loop, evutil_socket_t fd, 
                                        short flags, void *user_data) {
    if (!loop || !loop->base) {
        ws_log_error("Cannot create event: invalid event loop.");
        return NULL;
    }

    ws_event_t *ev = zcalloc(sizeof(ws_event_t));
    if (!ev) {
        ws_log_error("Failed to allocate memory for ws_event_t.");
        return NULL;
    }

    ev->loop = loop;
    ev->user_data = user_data;
    ev->evtype_flags = flags;

    // Determine which internal Libevent callback to use based on event flags
    // This is the fix: use Libevent's type directly for the function pointer.
    event_callback_fn cb = NULL; 
    if (flags & (WS_EV_READ | WS_EV_WRITE | WS_EV_ET)) {
        cb = s_event_io_cb;
    } else if (flags & WS_EV_TIMEOUT) {
        cb = s_event_timer_cb;
    } else {
        ws_log_error("Unsupported event flags %d.", (int)flags);
        zfree(ev);
        return NULL;
    }

    // event_new creates a new event struct and initializes it
    ev->ev = event_new(loop->base, fd, flags, cb, ev);
    if (!ev->ev) {
        ws_log_error("Failed to create libevent event (fd: %d, flags: %d).", fd, (int)flags);
        zfree(ev);
        return NULL;
    }

    ws_log_info("New ws_event_t %p created (fd %d, flags %d).", (void*)ev, fd, (int)flags);
    return ev;
}

ws_event_t *ws_event_new_io(ws_event_loop_t *loop, evutil_socket_t fd, short flags,
                            ws_io_callback_fn callback, void *user_data) {
    if (!(flags & (WS_EV_READ | WS_EV_WRITE)) || (flags & WS_EV_TIMEOUT)) {
        ws_log_error("Invalid flags for IO event. Must include WS_EV_READ/WS_EV_WRITE, not WS_EV_TIMEOUT.");
        return NULL;
    }

    ws_event_t *ev = s_ws_event_new_common(loop, fd, flags, user_data);
    if (ev) ev->cb.io_cb = callback;

    return ev;
}

ws_event_t *ws_event_new_timer(ws_event_loop_t *loop, long timeout_ms, bool is_persistent,
                               ws_timer_callback_fn callback, void *user_data) {
    if (timeout_ms < 0) {
        ws_log_error("Timer timeout_ms cannot be negative.");
        return NULL;
    }

    short flags = WS_EV_TIMEOUT;
    if (is_persistent) {
        flags |= WS_EV_PERSIST;
    }

    // -1 for fd on timers
    ws_event_t *ev = s_ws_event_new_common(loop, -1, flags, user_data);
    if (ev) {
        ev->cb.timer_cb = callback;
        ev->timeout_ms_val = timeout_ms;
    }

    return ev;
}

bool ws_event_add(ws_event_t *event) {
    if (!event || !event->ev) {
        ws_log_error("Cannot add NULL or invalid ws_event_t.");
        return false;
    }

    struct timeval tv;
    struct timeval *ptv = NULL;

    if (event->evtype_flags & WS_EV_TIMEOUT) {
        long current_timeout_ms = event->timeout_ms_val;
        if (current_timeout_ms == 0 && !(event->evtype_flags & WS_EV_PERSIST)) {
            // For non-persistent timers with 0 timeout, make it 1ms to ensure it schedules
            current_timeout_ms = 1;
        } else if (current_timeout_ms < 0) { 
            /* Should not happen if `ws_event_new_timer` checks
             * 0：Immediate timeout*/
            current_timeout_ms = 0;
        }

        tv.tv_sec = current_timeout_ms / 1000;
        tv.tv_usec = (current_timeout_ms % 1000) * 1000;
        ptv = &tv;
        ws_log_debug("Adding timer event for %ld ms.", current_timeout_ms);
    }

    if (event_add(event->ev, ptv) == -1) {
        ws_log_error("Failed to add event %p to event loop.", (void*)event);
        return false;
    }
    
    ws_log_debug("Event %p added to loop.", (void*)event);
    return true;
}

bool ws_event_del(ws_event_t *event) {
    if (!event || !event->ev) {
        ws_log_error("Cannot delete NULL or invalid ws_event_t.");
        return false;
    }

    if (event_del(event->ev) == -1) {
        ws_log_error("Failed to delete event %p from event loop.", (void*)event);
        return false;
    }

    ws_log_debug("Event %p deleted from loop.", (void*)event);
    return true;
}

void ws_event_free(ws_event_t *event) {
    if (!event) return;

    // Ensure event is not active before freeing its libevent counterpart
    // event_del returns 0 if event was active and removed, 1 if not active, -1 on error
    if (event_del(event->ev) != -1) {
        ws_log_debug("Event %p was inactive or successfully deleted before free.", (void*)event);
    }

    if (event->ev) {
        event_free(event->ev);
        event->ev = NULL;
    }

    zfree(event);
    ws_log_debug("ws_event_t %p freed.", (void*)event);
}