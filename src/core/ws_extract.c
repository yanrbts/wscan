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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>     // For strlen, strdup, memcpy
#include <pcre.h>       // For PCRE regex support
#include <curl/curl.h>  // For CURL support
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_util.h>
#include <ws_extract.h>

#define MIME_TEXT_HTML                          "text/html"
#define MIME_APPLICATION_JAVASCRIPT             "application/javascript"
#define MIME_APPLICATION_X_JAVASCRIPT           "application/x-javascript"
#define MIME_TEXT_JAVASCRIPT                    "text/javascript"
#define MIME_APPLICATION_X_SHOCKWAVE_FLASH      "application/x-shockwave-flash"

static JSRuntime *js_rt = NULL;
static JSContext *js_ctx = NULL;
static lxb_html_parser_t *parser = NULL;

static int add_link_to_array(extracted_links_t *links_data, const char *link) {
    if (!link) return -1;

    if (links_data->count >= links_data->capacity) {
        size_t new_capacity = links_data->capacity == 0 ? 4 : links_data->capacity * 2;
        char **new_links = zrealloc(links_data->links, sizeof(char*) * new_capacity);
        if (!new_links) {
            ws_log_error("Failed to reallocate links array");
            return -1; 
        }
        links_data->links = new_links;
        links_data->capacity = new_capacity;
    }

    links_data->links[links_data->count] = strdup(link);
    if (!links_data->links[links_data->count]) {
        ws_log_error("Failed to duplicate link string");
        return -1; // Memory error
    }
    links_data->count++;

    return 0;
}

static char *read_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        ws_log_error("Could not open file: %s", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *string = zmalloc(fsize + 1);
    if (!string) {
        fclose(f);
        return NULL;
    }
    fread(string, 1, fsize, f);
    fclose(f);
    string[fsize] = 0;

    return string;
}

static char *ws_get_domain_internal(const char *url) {
    char *host = NULL;
    char *domain = NULL;

    CURLU *h = curl_url();
    if (!h) {
        ws_log_warn("get_domain_c: Failed to create CURLU handle.");
        return NULL;
    }
    
    CURLUcode uc = curl_url_set(h, CURLUPART_URL, url, 0);
    if (uc == CURLUE_OK) {
        uc = curl_url_get(h, CURLUPART_HOST, &host, 0);
        if (uc == CURLUE_OK && host) {
            // Simple heuristic: if the hostname is an IP address, then the entire hostname is the domain
            if (strspn(host, "0123456789.") == strlen(host) && strchr(host, '.')) {
                domain = strdup(host);
            } else {
                /* For non-IP addresses, try to find the second-to-last dot as a simplified "top-level domain"
                 * For example: sub.example.com -> example.com
                 * But this would be wrong for example.co.uk (it would give co.uk instead of example.co.uk) */
                char *last_dot = strrchr(host, '.');
                if (last_dot) {
                    char *second_last_dot = NULL;
                    for (char *p = last_dot - 1; p >= host; p--) {
                        if (*p == '.') {
                            second_last_dot = p;
                            break;
                        }
                    }
                    if (second_last_dot && second_last_dot != host) {
                        domain = strdup(second_last_dot + 1);
                    } else {
                        domain = strdup(host);
                    }
                } else {
                    domain = strdup(host);
                }
            }
        }
    }
    if (host) curl_free(host);
    curl_url_cleanup(h);
    return domain;
}

static extracted_links_t *ws_extract_js_links_internal(const char *js_content, const char *base_url) {
    extracted_links_t *links_data = (extracted_links_t *)zmalloc(sizeof(extracted_links_t));
    if (!links_data) {
        ws_log_error("Failed to allocate extracted_links_t for JS parsing");
        return NULL;
    }
    links_data->links = NULL;
    links_data->count = 0;
    links_data->capacity = 0;

    if (!js_content || strlen(js_content) == 0) {
        ws_log_error("ws_extract_js_links_internal: Empty JavaScript content provided.");
        return links_data;
    }

    char *js_domain = ws_get_domain_internal(base_url);
    if (!js_domain) {
        ws_log_warn("Could not determine domain for JS base URL: %s", base_url);
        // Continue without domain filtering if domain cannot be determined
    }

    CURLU *base_url_parts = curl_url();
    char *base_scheme = NULL;
    char *base_netloc = NULL;
    if (base_url_parts && curl_url_set(base_url_parts, CURLUPART_URL, base_url, 0) == CURLUE_OK) {
        curl_url_get(base_url_parts, CURLUPART_SCHEME, &base_scheme, 0);
        curl_url_get(base_url_parts, CURLUPART_HOST, &base_netloc, 0);
    } else {
        ws_log_warn("Failed to parse base URL for JS extraction: %s", base_url);
    }

    const char *path_patterns[] = {
        "(?:path|redirectTo|templateUrl)[\"']?:\\s?[\"']([^\n\"'+*$(]*)[\"']",
        "\\[\"(?:href|src)\"\\],\\s?[\"']([^\n\"'(:]*)[\"']",
        "router\\.(?:navigateByUrl|parseUrl|isActive)\\([\\w\\s.+]*[\"']([^\n\"']*?)[\"'].*?\\)",
        NULL
    };

    const char *path_with_params_pattern = "router\\.(?:navigate|createUrlTree)\\(\\[[\\w\\s]*[\"']([^\n\"']*?)[\"'].*?\\](?:.*?)\\)";
    const char *full_url_pattern = "https?://[^\\s\"'\\\\)]+";

    pcre *re;
    const char *error;
    int erroffset;
    int rc;
    int ovector[30]; // For capturing groups (max 15 pairs)

    // Collect paths and full URLs separately first
    extracted_links_t *temp_paths = (extracted_links_t *)zmalloc(sizeof(extracted_links_t));
    if (!temp_paths) goto cleanup_js;
    temp_paths->links = NULL; 
    temp_paths->count = 0; 
    temp_paths->capacity = 0;

    extracted_links_t *temp_full_urls = (extracted_links_t *)zmalloc(sizeof(extracted_links_t));
    if (!temp_full_urls) { 
        ws_free_extracted_links(temp_paths); 
        goto cleanup_js; 
    }
    temp_full_urls->links = NULL; 
    temp_full_urls->count = 0; 
    temp_full_urls->capacity = 0;

    // --- Process Path Patterns ---
    for (int i = 0; path_patterns[i] != NULL; i++) {
        re = pcre_compile(path_patterns[i], PCRE_DOTALL | PCRE_UNGREEDY, &error, &erroffset, NULL);
        if (!re) {
            ws_log_error("PCRE compilation failed at offset %d: %s for pattern %s", erroffset, error, path_patterns[i]);
            continue;
        }

        const char *current_js_content = js_content;
        int current_offset = 0;
        int content_len = strlen(js_content);

        while ((rc = pcre_exec(re, NULL, current_js_content, content_len - current_offset, current_offset, 0, ovector, 30)) >= 0) {
            if (rc > 0) { 
                // Found at least one capture group
                int start = ovector[2]; // Start of first captured group (P<path>)
                int end = ovector[3];   // End of first captured group

                char *path_str = (char *)zmalloc(end - start + 1);
                if (path_str) {
                    memcpy(path_str, current_js_content + start, end - start);
                    path_str[end - start] = '\0';
                    if (strlen(path_str) > 0 && strstr(path_str, "http") == NULL) { // Only add if not full URL
                        add_link_to_array(temp_paths, path_str);
                    }
                    zfree(path_str);
                }
            }
            current_offset = ovector[1]; // Move past the current match
        }
        pcre_free(re);
    }

    /* --- Process Path with Params Pattern (simplified) ---
     * This part is very tricky due to Python's re.sub. A direct translation is hard.
     * We'll extract the raw path part and add it. */
    re = pcre_compile(path_with_params_pattern, PCRE_DOTALL | PCRE_UNGREEDY, &error, &erroffset, NULL);
    if (!re) {
        ws_log_error("PCRE compilation failed for path_with_params: %s", error);
    } else {
        const char *current_js_content = js_content;
        int current_offset = 0;
        int content_len = strlen(js_content);
        while ((rc = pcre_exec(re, NULL, current_js_content, content_len - current_offset, current_offset, 0, ovector, 30)) >= 0) {
            if (rc > 0) {
                int start = ovector[2];
                int end = ovector[3];
                char *path_raw = (char *)zmalloc(end - start + 1);
                if (path_raw) {
                    memcpy(path_raw, current_js_content + start, end - start);
                    path_raw[end - start] = '\0';
                    if (strlen(path_raw) > 0 && strstr(path_raw, "http") == NULL) {
                        add_link_to_array(temp_paths, path_raw);
                    }
                    zfree(path_raw);
                }
            }
            current_offset = ovector[1];
        }
        pcre_free(re);
    }

    // --- Process Full URL Pattern ---
    re = pcre_compile(full_url_pattern, PCRE_DOTALL | PCRE_UNGREEDY, &error, &erroffset, NULL);
    if (!re) {
        ws_log_error("PCRE compilation failed for full_url: %s", error);
    } else {
        const char *current_js_content = js_content;
        int current_offset = 0;
        int content_len = strlen(js_content);
        while ((rc = pcre_exec(re, NULL, current_js_content, content_len - current_offset, current_offset, 0, ovector, 30)) >= 0) {
            if (rc > 0) {
                int start = ovector[0]; // Full match
                int end = ovector[1];
                char *full_url_str = (char *)zmalloc(end - start + 1);
                if (full_url_str) {
                    memcpy(full_url_str, current_js_content + start, end - start);
                    full_url_str[end - start] = '\0';
                    // Check if domain matches target domain
                    char *found_domain = ws_get_domain_internal(full_url_str);
                    if (found_domain && js_domain && strcmp(found_domain, js_domain) == 0) {
                        add_link_to_array(temp_full_urls, full_url_str);
                    } else {
                         ws_log_debug("Skipping full URL out of domain: %s (found_domain: %s, target_domain: %s)", full_url_str, found_domain, js_domain);
                    }
                    if (found_domain) zfree(found_domain);
                    zfree(full_url_str);
                }
            }
            current_offset = ovector[1];
        }
        pcre_free(re);
    }

    // --- 组合链接 (Python 的 for url in domain_found: for path in path_found:) ---
    // 首先添加所有直接发现的完整 URL
    for (size_t i = 0; i < temp_full_urls->count; i++) {
        add_link_to_array(links_data, temp_full_urls->links[i]);
    }

    // 然后组合 domain_found 和 path_found
    // Python 逻辑是：对每个 domain_found 中的 URL，与 path_found 中的每个 path 组合
    // 这里简化为：对 base_url 的 domain，与所有 path 组合
    if (base_scheme && base_netloc) {
        for (size_t i = 0; i < temp_paths->count; i++) {
            const char *path = temp_paths->links[i];
            char *new_url = NULL;
            if (path && strlen(path) > 0) {
                if (path[0] != '/') {
                    new_url = (char *)zmalloc(strlen(base_scheme) + 3 + strlen(base_netloc) + 1 + strlen(path) + 1);
                    if (new_url) {
                        sprintf(new_url, "%s://%s/%s", base_scheme, base_netloc, path);
                    }
                } else {
                    new_url = (char *)zmalloc(strlen(base_scheme) + 3 + strlen(base_netloc) + strlen(path) + 1);
                    if (new_url) {
                        sprintf(new_url, "%s://%s%s", base_scheme, base_netloc, path);
                    }
                }
                if (new_url) {
                    add_link_to_array(links_data, new_url);
                    zfree(new_url);
                }
            }
        }
    }

cleanup_js:
    if (js_domain) zfree(js_domain);
    if (base_scheme) curl_free(base_scheme);
    if (base_netloc) curl_free(base_netloc);
    if (base_url_parts) curl_url_cleanup(base_url_parts);
    if (temp_paths) ws_free_extracted_links(temp_paths);
    if (temp_full_urls) ws_free_extracted_links(temp_full_urls);

    return links_data;
}

int ws_extract_init(void) {
    js_rt = JS_NewRuntime();
    if (!js_rt) {
        ws_log_error("Failed to create QuickJS runtime");
        return -1;
    }

    js_ctx = JS_NewContext(js_rt);
    if (!js_ctx) {
        ws_log_error("Failed to create QuickJS context");
        JS_FreeRuntime(js_rt);
        js_rt = NULL;
        return -1;
    }

    parser = lxb_html_parser_create();
    if (!parser) {
        ws_log_error("Failed to create Lexbor HTML parser.");
        JS_FreeContext(js_ctx);
        JS_FreeRuntime(js_rt);
        js_ctx = NULL;
        js_rt = NULL;
        return -1;
    }

    return 0;
}

char *ws_extract_data(const char *html_content, size_t html_len, const char *script_path) {
    if (!js_ctx) {
        ws_log_error("ws_extract_data: QuickJS not initialized. Call ws_extract_init() first.");
        return NULL;
    }

    char *js_code = read_file(script_path);
    if (!js_code) return NULL;

    JSValue global_obj = JS_GetGlobalObject(js_ctx);

    // 将 HTML 内容作为字符串传递给 QuickJS
    JS_SetPropertyStr(js_ctx, global_obj, "htmlContent", JS_NewStringLen(js_ctx, html_content, html_len));
    // 执行 JavaScript 脚本
    JSValue val = JS_Eval(js_ctx, js_code, strlen(js_code), script_path, JS_EVAL_TYPE_MODULE);
    // 释放 JS 代码缓冲区
    zfree(js_code);

    char *extracted_data = NULL;

    if (JS_IsException(val)) {
        JS_GetException(js_ctx); // 打印异常以便调试
        ws_log_error("QuickJS script execution failed.");
    } else if (JS_IsString(val)) {
        // 假设 JavaScript 脚本返回一个字符串
        size_t len;
        const char *str = JS_ToCStringLen(js_ctx, &len, val);
        if (str) {
            extracted_data = strdup(str);
            JS_FreeCString(js_ctx, str);
        }
    } else {
        ws_log_warn("QuickJS script did not return a string (returned type: %d).", JS_VALUE_GET_TAG(val));
    }

    JS_FreeValue(js_ctx, val); // 释放 JS 值
    JS_FreeValue(js_ctx, global_obj); // 释放全局对象引用

    return extracted_data;
}

extracted_links_t *ws_extract_links(const char *html_content, size_t html_len, 
                                    const char *content_type, const char *base_url) {
    
    if (!html_content || html_len == 0 || !content_type || !base_url) {
        ws_log_error("ws_extract_links: Empty HTML content provided.");
        return NULL;
    }

    extracted_links_t *links_data = (extracted_links_t *)zmalloc(sizeof(extracted_links_t));
    if (!links_data) {
        ws_log_error("Failed to allocate extracted_links_t");
        return NULL;
    }
    links_data->links = NULL;
    links_data->count = 0;
    links_data->capacity = 0;

    if (ws_strcheck_prefix(content_type, MIME_TEXT_HTML)) {
        if (!parser) {
            ws_log_error("ws_extract_links: Lexbor parser not initialized. Call ws_extract_init() first.");
            return NULL;
        }

        lxb_status_t status;
        lxb_dom_collection_t *collection = NULL;
        lxb_html_document_t *document = NULL;
        lxb_dom_document_t *dom_document = NULL;
        lxb_dom_element_t *root = NULL;

        document = lxb_html_document_create();
        if (!document) {
            ws_log_error("Failed to create HTML Document.");
            goto err;
        }

        status = lxb_html_document_parse(document, (const lxb_char_t *)html_content, html_len);
        if (status != LXB_STATUS_OK) {
            ws_log_error("Failed to parse HTML");
            goto err;
        }

        dom_document = lxb_html_document_original_ref_noi(document);
        collection = lxb_dom_collection_make_noi(dom_document, 128);
        if (collection == NULL) {
            ws_log_error("Failed to create collection");
            goto err;
        }

        root = lxb_dom_document_element(dom_document);
        status = lxb_dom_elements_by_tag_name(root, collection, (const lxb_char_t *)"a", 1);
        if (status != LXB_STATUS_OK) {
            ws_log_error("Failed to get elements by tag name 'a'");
            goto err;
        }

        for (size_t i = 0; i < lxb_dom_collection_length_noi(collection); i++) {
            lxb_dom_element_t *element = lxb_dom_collection_element_noi(collection, i);
            size_t value_len;
            const lxb_char_t *href = lxb_dom_element_get_attribute(element, (const lxb_char_t *)"href", 4, &value_len);
            if (href && value_len > 0) {
                add_link_to_array(links_data, (const char *)href);
            }
        }

        lxb_dom_collection_destroy(collection, true);
        lxb_html_document_destroy(document);

        return links_data;

    err:
        if (collection) lxb_dom_collection_destroy(collection, true);
        if (document) lxb_html_document_destroy(document);
        if (links_data) ws_free_extracted_links(links_data);
        return NULL;
    } else if (ws_strcheck_prefix(content_type, MIME_APPLICATION_JAVASCRIPT) ||
             ws_strcheck_prefix(content_type, MIME_APPLICATION_X_JAVASCRIPT) ||
             ws_strcheck_prefix(content_type, MIME_TEXT_JAVASCRIPT)) {
        ws_log_debug("Attempting JavaScript link extraction for URL: %s (Type: %s)", base_url, content_type);
        return ws_extract_js_links_internal(html_content, base_url);
    } else if (ws_strcheck_prefix(content_type, MIME_APPLICATION_X_SHOCKWAVE_FLASH)) {
        // TODO: SWF 链接提取需要一个专门的 SWF 解析库，例如libflashparser或者自行实现。
        // QuickJS 和 Lexbor 都无法直接处理 SWF 二进制内容。
        ws_log_warn("SWF link extraction not implemented. Requires a dedicated SWF parser. URL: %s (Type: %s)", base_url, content_type);
        return links_data;
    } else {
        ws_log_debug("Unhandled content type '%s'. Skipping link extraction for URL: %s", content_type, base_url);
        return links_data;
    }
}

void ws_free_extracted_links(extracted_links_t *links_data) {
    if (links_data) {
        for (size_t i = 0; i < links_data->count; i++) {
            zfree(links_data->links[i]); 
        }
        zfree(links_data->links); 
        zfree(links_data);
    }
}

void ws_extract_cleanup(void) {
    if (parser) {
        lxb_html_parser_destroy(parser);
        parser = NULL;
    }

    if (js_ctx) {
        JS_FreeContext(js_ctx);
        js_ctx = NULL;
    }

    if (js_rt) {
        JS_FreeRuntime(js_rt);
        js_rt = NULL;
    }
}