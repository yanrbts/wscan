#include <stdio.h>
#include <stdlib.h> // For EXIT_SUCCESS, EXIT_FAILURE
#include <ws_explorer.h>

int main() {
    printf("Starting ws_explorer...\n");

    // 初始化爬虫：最大深度 2，最大页面大小 1MB，并发请求数 5
    ws_explorer *explorer = ws_explorer_new(10, 1024 * 1024, 10);
    if (!explorer) {
        fprintf(stderr, "Failed to create ws_explorer.\n");
        return EXIT_FAILURE;
    }

    // 创建一个初始请求
    ws_request *initial_request = ws_request_new(
        "https://www.jd.com/", "GET", 0, // URL, Method, Depth
        NULL, NULL, 0, false, NULL, 0,     // POST data (none)
        NULL, NULL, NULL); // Content-Type, Referer, User Data (will be set by explorer)

    if (!initial_request) {
        fprintf(stderr, "Failed to create initial request.\n");
        ws_explorer_free(explorer);
        return EXIT_FAILURE;
    }

    // 开始爬取
    ws_explorer_explore(explorer, initial_request);

    printf("\n--- Exploration Summary ---\n");
    printf("Total requests issued: %d\n", explorer->event_base->total_requests);
    printf("Successfully completed requests: %d\n", explorer->event_base->success_requests);
    printf("Failed requests: %d\n", explorer->event_base->failed_requests);
    // printf("Visited unique URLs: %zu\n", explorer->visited_urls->size);


    // 清理
    ws_explorer_free(explorer);
    printf("ws_explorer finished and cleaned up.\n");

    return EXIT_SUCCESS;
}