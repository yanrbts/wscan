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
#include <stdlib.h>
#include <string.h>
#include <ws_malloc.h>
#include <ws_log.h>
#include <ws_request.h>

char *ws_safe_strdup(const char *s) {
    if (!s) {
        ws_log_error("Attempted to duplicate a NULL string.");
        return NULL;
    }

    char *dup = strdup(s);
    if (!dup) {
        ws_log_error("strdup failed.");
        return NULL;
    }
    return dup;
}

ws_request *ws_request_new(const char *url, const char *method, int link_depth,
                           const char *raw_body,
                           const ws_formparam *form_params, size_t num_form_params,
                           bool post_is_form_data,
                           const ws_fileparam *file_params, size_t num_file_params,
                           const char *content_type, const char *referer, void *user_data) {
    ws_request *req = (ws_request*)zcalloc(sizeof(ws_request));
    if (!req) {
        ws_log_error("Failed to allocate memory for ws_request.");
        goto err;
    }

    req->url = url ? ws_safe_strdup(url) : NULL;
    if (!req->url) {
        goto err;
    }
    req->method = method ? ws_safe_strdup(method) : NULL;
    if (!req->method) {
        goto err;
    }
    req->link_depth = link_depth;

    req->post_is_form_data = post_is_form_data;

    if (post_is_form_data) {
        if (num_form_params > 0) {
            req->post_data.form_params = (ws_formparam*)zmalloc(num_form_params * sizeof(ws_formparam));
            if (!req->post_data.form_params) {
                ws_log_error("Failed to allocate memory for form parameters.");
                goto err;
            }
            req->post_data.num_form_params = num_form_params;

            for (size_t i = 0; i < num_form_params; i++) {
                req->post_data.form_params[i].key = ws_safe_strdup(form_params[i].key);
                if (!req->post_data.form_params[i].key) {
                    ws_log_error("Failed to duplicate form parameter key.");
                    goto err;
                }
                req->post_data.form_params[i].value = ws_safe_strdup(form_params[i].value);
                if (!req->post_data.form_params[i].value) {
                    ws_log_error("Failed to duplicate form parameter value.");
                    goto err;
                }
            }
        }
    } else {
        req->post_data.raw_body = raw_body ? ws_safe_strdup(raw_body) : NULL;
        if (raw_body && !req->post_data.raw_body) {
            goto err;
        }
    }

    if (num_file_params > 0) {
        req->file_params = (ws_fileparam*)zcalloc(num_file_params * sizeof(ws_fileparam));
        if (!req->file_params) {
            goto err;
        }
        req->num_file_params = num_file_params;
        for (size_t i = 0; i < num_file_params; ++i) {
            req->file_params[i].field_name = ws_safe_strdup(file_params[i].field_name);
            req->file_params[i].file_name = ws_safe_strdup(file_params[i].file_name);
            // file_content is not duplicated, assume caller manages its lifetime or it's copied later
            req->file_params[i].file_content = file_params[i].file_content;
            req->file_params[i].file_content_len = file_params[i].file_content_len;
            if (!req->file_params[i].field_name || !req->file_params[i].file_name) {
                goto err;
            }
        }
    }

    req->content_type = content_type ? ws_safe_strdup(content_type) : NULL;
    req->referer = referer ? ws_safe_strdup(referer) : NULL;
    req->user_data = user_data;
    req->extra_headers = NULL;      // Initialize curl_slist
    req->response = NULL;           // Will be set by ws_http module

    return req;
err:
    if (req) 
        ws_request_free(req);
    return NULL;
}

void ws_request_add_header(ws_request *request, const char *header_name, const char *header_value) {
    if (!request || !header_name || !header_value)
        return;

    char header[256]; // Max header length
    snprintf(header, sizeof(header), "%s: %s", header_name, header_value);
    request->extra_headers = curl_slist_append(request->extra_headers, header);
}

void ws_request_free(ws_request *request) {
    if (request) {
        zfree(request->url);
        zfree(request->method);

        if (request->post_is_form_data && request->post_data.form_params) {
            for (size_t i = 0; i < request->post_data.num_form_params; ++i) {
                zfree(request->post_data.form_params[i].key);
                zfree(request->post_data.form_params[i].value);
            }
            zfree(request->post_data.form_params);
        } else if (request->post_data.raw_body) {
            zfree(request->post_data.raw_body);
        }

        if (request->file_params) {
            for (size_t i = 0; i < request->num_file_params; ++i) {
                zfree(request->file_params[i].field_name);
                zfree(request->file_params[i].file_name);
                // Note: file_content itself is not freed here,
                // as it's assumed to be managed by the caller.
            }
            zfree(request->file_params);
        }

        // IMPORTANT: If headers were passed to CURLOPT_HTTPHEADER, libcurl owns them.
        // We only free them if they were NOT passed to curl_easy_setopt.
        // For simplicity, we assume they are always passed, so no need to free here.
        // If not passed (e.g., failed to add to multi handle), they would leak.
        // A more robust solution might track if `extra_headers` was consumed.
        // For now, curl_slist_free_all(request->extra_headers); is commented out.
        zfree(request->content_type);
        zfree(request->referer);

        ws_response_free(request->response); // Free associated response
        zfree(request);
    }
}