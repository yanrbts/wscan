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
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_extract.h>

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

extracted_links_t *ws_extract_links(const char *html_content, size_t html_len) {
    if (!parser) {
        ws_log_error("ws_extract_links: Lexbor parser not initialized. Call ws_extract_init() first.");
        return NULL;
    }

    if (!html_content || html_len == 0) {
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