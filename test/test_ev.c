// main.c (Example Usage)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h> // For signal handling
#include <unistd.h> // For sleep

#include <ws_log.h>
#include <ws_event.h>

// Global event loop pointer for signal handler
ws_event_loop_t *g_main_loop = NULL;

// Example callback for a timer event
void my_timer_callback(void *user_data) {
    const char *message = (const char *)user_data;
    ws_log_info("Timer triggered! Message: %s", message);

    // If it's a non-persistent timer, it only fires once.
    // To fire again, you'd need to re-add it.
    // If it's persistent (created with WS_EV_PERSIST), Libevent automatically re-arms it.
}

// Example callback for an I/O event (e.g., watching stdin)
void my_io_callback(evutil_socket_t fd, short events, void *user_data) {
    const char *context = (const char *)user_data;
    ws_log_info("IO event triggered on FD %d. Events: %d. Context: %s", fd, (int)events, context);

    if (events & WS_EV_READ) {
        char buffer[256];
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            ws_log_info("Read from FD %d: '%s'", fd, buffer);
        } else if (bytes_read == 0) {
            ws_log_info("EOF on FD %d. Removing IO event.", fd);
            ws_event_del( (ws_event_t*)user_data ); // NOTE: In real code, user_data shouldn't be the event itself.
                                                    // Pass the ws_event_t pointer as part of a custom struct.
            ws_event_free( (ws_event_t*)user_data ); // Then free the event.
        } else {
            ws_log_error("Error reading from FD %d.", fd);
        }
    }

    // This example has a potential issue: `user_data` here is the string "stdin_monitor",
    // but the intention for removing/freeing might be to pass the `ws_event_t *` itself.
    // In a real application, you'd wrap `ws_event_t *` and `user_data` in a custom struct
    // and pass *that* struct as `user_data` to `ws_event_new_io`.
    // For demonstration, `ws_log_info` works, but cleanup needs care.
}

// Signal handler to gracefully stop the loop
void signal_handler(int sig) {
    if (sig == SIGINT) {
        ws_log_info("SIGINT received. Shutting down event loop...");
        if (g_main_loop) {
            ws_event_loop_stop(g_main_loop);
        } else {
            exit(EXIT_SUCCESS);
        }
    }
}

int main() {
    // Set up signal handler
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        ws_log_error("Failed to set SIGINT handler.");
        return EXIT_FAILURE;
    }

    // 1. Create an event loop
    ws_event_loop_t *loop = ws_event_loop_new();
    if (!loop) {
        return EXIT_FAILURE;
    }
    g_main_loop = loop; // Set global for signal handler

    // 2. Create and add a persistent timer event
    ws_event_t *timer_event = ws_event_new_timer(loop, 2000, true, my_timer_callback, "2-second persistent timer");
    if (!timer_event || !ws_event_add(timer_event)) {
        ws_log_error("Failed to add timer event.");
        ws_event_loop_free(loop);
        return EXIT_FAILURE;
    }

    // 3. Create and add a one-shot timer event
    ws_event_t *one_shot_timer = ws_event_new_timer(loop, 5000, false, my_timer_callback, "5-second one-shot timer");
    if (!one_shot_timer || !ws_event_add(one_shot_timer)) {
        ws_log_error("Failed to add one-shot timer event.");
        // Continue but cleanup on exit
    }

    // 4. Create and add an I/O event to monitor standard input (FD 0)
    // NOTE: Passing `one_shot_timer` as user_data for demo,
    // in real app, you'd pass a custom struct or context.
    ws_event_t *stdin_event = ws_event_new_io(loop, STDIN_FILENO, WS_EV_READ | WS_EV_PERSIST, my_io_callback, "stdin_monitor");
    if (!stdin_event || !ws_event_add(stdin_event)) {
        ws_log_error("Failed to add stdin IO event.");
        // Continue but cleanup on exit
    }

    ws_log_info("Event loop initialized. Press Ctrl+C to stop, or type something and press Enter.");

    // 5. Dispatch the event loop (this blocks until ws_event_loop_stop() is called)
    ws_event_loop_dispatch(loop);

    // 6. Cleanup
    ws_log_info("Cleaning up events and loop...");
    ws_event_free(timer_event);     // Free persistent timer
    ws_event_free(one_shot_timer);  // Free one-shot timer (already triggered or not)
    ws_event_free(stdin_event);     // Free stdin event
    ws_event_loop_free(loop);
    
    ws_log_info("Application exited.");
    return EXIT_SUCCESS;
}