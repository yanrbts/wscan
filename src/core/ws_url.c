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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <libgen.h> // For dirname, basename
#include <ws_url.h>

// Helper for string duplication
static char* url_safe_strdup(const char *s) {
    if (!s) return NULL;
    char *dup = strdup(s);
    if (!dup) {
        perror("strdup failed in url_safe_strdup");
    }
    return dup;
}

// 简单的 URL 解析函数，提取协议、主机、端口和路径
// 这不是一个完整的 URL 解析器，仅用于内部辅助
static void parse_url_parts(const char *url, char **protocol, char **hostname, int *port, char **path) {
    *protocol = NULL;
    *hostname = NULL;
    *port = 0; // 0 means default port
    *path = NULL;

    char *url_copy = url_safe_strdup(url);
    if (!url_copy) return;

    char *temp = url_copy;
    char *p_end = strstr(temp, "://");
    if (p_end) {
        *protocol = strndup(temp, p_end - temp);
        temp = p_end + 3; // Skip "://"
    } else {
        // No protocol, assume http or https if port is 443
        // This is a simplification; in real world, one might default to "http"
        // Or leave protocol as NULL and let other logic determine.
        *protocol = url_safe_strdup("http"); // Default to http if no protocol
    }

    char *host_end = strchr(temp, '/');
    char *port_sep = strchr(temp, ':');
    char *query_sep = strchr(temp, '?');
    char *fragment_sep = strchr(temp, '#');

    char *effective_end = NULL;
    if (host_end) effective_end = host_end;
    if (port_sep && (effective_end == NULL || port_sep < effective_end)) effective_end = port_sep;
    if (query_sep && (effective_end == NULL || query_sep < effective_end)) effective_end = query_sep;
    if (fragment_sep && (effective_end == NULL || fragment_sep < effective_end)) effective_end = fragment_sep;

    if (effective_end) {
        *hostname = strndup(temp, effective_end - temp);
    } else {
        *hostname = url_safe_strdup(temp);
    }

    if (port_sep && (port_sep < (host_end ? host_end : (char*)(size_t)-1))) {
        // Port specified
        char *port_str_start = port_sep + 1;
        char *port_str_end = (host_end && host_end < query_sep ? host_end : query_sep);
        if (!port_str_end && fragment_sep && fragment_sep > port_sep) port_str_end = fragment_sep;
        if (!port_str_end) port_str_end = temp + strlen(temp);

        char *port_val_str = strndup(port_str_start, port_str_end - port_str_start);
        if (port_val_str) {
            *port = atoi(port_val_str);
            free(port_val_str);
        }
    }

    if (host_end) {
        // Path starts after hostname
        char *path_start = host_end;
        char *path_end = (query_sep && query_sep < fragment_sep ? query_sep : fragment_sep);
        if (!path_end) path_end = temp + strlen(temp);
        if (path_end) {
            *path = strndup(path_start, path_end - path_start);
        } else {
            *path = url_safe_strdup(path_start);
        }
    } else {
        *path = url_safe_strdup("/"); // Default path if no slash
    }

    free(url_copy);
}

char* ws_url_get_fld(const char *url) {
    char *protocol = NULL;
    char *hostname = NULL;
    int port;
    char *path = NULL;
    parse_url_parts(url, &protocol, &hostname, &port, &path);

    char *fld = NULL;
    if (hostname) {
        // Simple FLD extraction: find last two dots
        // This is a naive approach and is NOT robust for all TLDs (e.g., .co.uk)
        // A proper FLD requires a Public Suffix List.
        char *dot1 = strrchr(hostname, '.');
        if (dot1) {
            char *dot2 = strrchr(hostname, '.'); // find second last dot
            if (dot2 && dot2 != dot1) {
                char *temp = dot2;
                // Move back to find the start of the "domain.tld" part
                int count = 0;
                while(temp > hostname && *temp == '.') { // Handle multiple dots like .com.cn
                    temp--;
                    count++;
                    if (count > 1) break; // Simplified: only look for .tld or .sub.tld
                }
                if (count > 1 && temp > hostname && *temp != '.') { // Found a second dot, potentially a real domain
                    char *start = strrchr(hostname, '.'); // First dot from right
                    if (start) {
                        char *second_start = strrchr(hostname, '.');
                        if (second_start && second_start != start) { // Has at least two dots
                            // Handle cases like .co.uk. For simplicity, just get everything after the first dot from right
                            // For www.google.com, start points to .com. We need google.com
                            // For news.bbc.co.uk, start points to .uk. We need bbc.co.uk
                            // A proper check needs a public suffix list.
                            // For now, let's just grab the last two segments if available.
                            char *first_dot = strchr(hostname, '.'); // First dot from left
                            char *last_dot = strrchr(hostname, '.'); // Last dot from right
                            if (first_dot && last_dot && first_dot != last_dot) {
                                // If there are multiple dots, take the part after the first dot from the end.
                                // e.g., "www.google.com" -> "google.com"
                                // e.g., "sub.domain.co.uk" -> "co.uk" (incorrect for this naive approach)
                                // e.g., "sub.domain.com" -> "domain.com"
                                char *fld_start = last_dot; // start from .tld
                                // Move back to find the start of the domain part
                                while(fld_start > hostname && *(fld_start - 1) != '.') {
                                    fld_start--;
                                }
                                fld = url_safe_strdup(fld_start);
                            } else {
                                // If only one dot (e.g. "localhost.com") or no dots ("localhost")
                                fld = url_safe_strdup(hostname);
                            }
                        } else { // Only one dot like "example.com"
                            char *start = strrchr(hostname, '.');
                            if (start) {
                                while(start > hostname && *(start-1) != '.') start--;
                                fld = url_safe_strdup(start);
                            } else { // No dot, like "localhost"
                                fld = url_safe_strdup(hostname);
                            }
                        }
                    } else { // No dot, like "localhost"
                         fld = url_safe_strdup(hostname);
                    }
                } else { // Only one dot
                    char *start = strrchr(hostname, '.');
                    if (start) {
                        while(start > hostname && *(start-1) != '.') start--;
                        fld = url_safe_strdup(start);
                    } else { // No dot
                        fld = url_safe_strdup(hostname);
                    }
                }
            } else { // Only one dot, or no dots
                fld = url_safe_strdup(hostname);
            }
        } else { // No dots at all (e.g. "localhost")
            fld = url_safe_strdup(hostname);
        }
    }

    free(protocol);
    free(hostname);
    free(path);
    return fld;
}


char* ws_url_get_hostname(const char *url) {
    char *protocol = NULL;
    char *hostname = NULL;
    int port;
    char *path = NULL;
    parse_url_parts(url, &protocol, &hostname, &port, &path);

    // Free parsed parts not needed for return
    free(protocol);
    free(path);

    return hostname; // This is the hostname
}

// 简化的 URL 路径拼接
char* ws_url_resolve(const char *base_url, const char *relative_url) {
    if (!base_url || !relative_url) return NULL;

    // 如果 relative_url 是绝对路径，直接返回其副本
    if (strstr(relative_url, "://") != NULL || strncmp(relative_url, "//", 2) == 0) {
        return url_safe_strdup(relative_url);
    }

    char *base_copy = url_safe_strdup(base_url);
    if (!base_copy) return NULL;

    char *protocol = NULL;
    char *hostname = NULL;
    int port;
    char *path = NULL;
    parse_url_parts(base_copy, &protocol, &hostname, &port, &path);

    if (!protocol || !hostname) {
        free(base_copy);
        free(protocol); free(hostname); free(path);
        return NULL;
    }

    // 确定基础路径
    char *base_path_temp = url_safe_strdup(path);
    if (!base_path_temp) {
        free(base_copy);
        free(protocol); free(hostname); free(path);
        return NULL;
    }

    char *resolved_path = NULL;
    if (relative_url[0] == '/') {
        // 相对 URL 以 / 开始，表示根路径下的绝对路径
        resolved_path = url_safe_strdup(relative_url);
    } else {
        // 相对路径，相对于当前目录
        char *dir_name = dirname(base_path_temp); // dirname 会修改其参数
        size_t dir_len = strlen(dir_name);
        size_t rel_len = strlen(relative_url);
        size_t total_len = dir_len + 1 + rel_len + 1; // +1 for slash, +1 for null terminator
        resolved_path = (char*)malloc(total_len);
        if (!resolved_path) {
            perror("malloc failed for resolved_path");
            free(base_path_temp);
            free(protocol); free(hostname); free(path);
            return NULL;
        }
        snprintf(resolved_path, total_len, "%s/%s", dir_name, relative_url);
    }

    // 重新构建完整 URL
    size_t result_len = 0;
    char *resolved_url = NULL;

    // 计算长度
    result_len += strlen(protocol);
    result_len += 3; // "://"
    result_len += strlen(hostname);
    if (port != 0 &&
        !((strcmp(protocol, "http") == 0 && port == 80) ||
          (strcmp(protocol, "https") == 0 && port == 443))) {
        result_len += 1; // ":"
        result_len += snprintf(NULL, 0, "%d", port); // Port number length
    }
    result_len += strlen(resolved_path);
    result_len += 1; // Null terminator

    resolved_url = (char*)malloc(result_len);
    if (!resolved_url) {
        perror("malloc failed for resolved_url");
        free(resolved_path);
        free(base_path_temp);
        free(protocol); free(hostname); free(path);
        return NULL;
    }

    char *current_pos = resolved_url;
    strcpy(current_pos, protocol);
    current_pos += strlen(protocol);
    strcpy(current_pos, "://");
    current_pos += 3;
    strcpy(current_pos, hostname);
    current_pos += strlen(hostname);

    if (port != 0 &&
        !((strcmp(protocol, "http") == 0 && port == 80) ||
          (strcmp(protocol, "https") == 0 && port == 443))) {
        current_pos += snprintf(current_pos, resolved_url + result_len - current_pos, ":%d", port);
    }
    strcpy(current_pos, resolved_path);

    free(base_copy); // dirname might modify it, so free original copy
    free(protocol);
    free(hostname);
    free(path); // Original path
    free(base_path_temp); // dirname temp
    free(resolved_path);

    return resolved_url;
}