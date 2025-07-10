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
#ifndef __WS_EXTRACT_H__
#define __WS_EXTRACT_H__

#include <stddef.h>                         // For size_t
#include <lexbor/html/parser.h>             // Lexbor HTML parser headers
#include <lexbor/dom/interfaces/element.h>  // For element operations
#include <lexbor/dom/interfaces/node.h>     // For node operations
#include <lexbor/html/serialize.h>
#include <lexbor/html/interfaces/document.h>
#include <quickjs/quickjs.h>


/**
 * @brief Structure to store a list of extracted links.
 *
 * The structure holds an array of pointers to strings (links), the current number of links,
 * and the current capacity of the array.
 */
typedef struct extracted_links {
    char **links;      ///< Array of pointers to link strings.
    size_t count;      ///< Current number of links stored.
    size_t capacity;   ///< Current capacity of the array.
} extracted_links_t;

/**
 * @brief Initialize the extraction module (QuickJS and Lexbor).
 *
 * This function must be called before using any extraction functions.
 * It initializes the required resources for HTML parsing and JavaScript execution.
 *
 * @return 0 on success, non-zero on failure.
 */
int ws_extract_init(void);

/**
 * @brief Extract generic data from HTML content using QuickJS and a user script.
 *
 * This function executes a JavaScript script (provided by script_path) on the given HTML content.
 * It is optional and only needed if you require script-based extraction.
 *
 * @param html_content Pointer to the HTML content buffer.
 * @param html_len     Length of the HTML content.
 * @param script_path  Path to the JavaScript extraction script.
 * @return A newly allocated string containing the extracted data, or NULL on failure.
 *         The caller is responsible for freeing the returned string.
 */
char *ws_extract_data(const char *html_content, size_t html_len, const char *script_path);

/**
 * @brief Extract all links from HTML content using Lexbor.
 *
 * This function parses the HTML content and extracts all links (e.g., <a href="...">).
 *
 * @param html_content Pointer to the HTML content buffer.
 * @param html_len     Length of the HTML content.
 * @return Pointer to an extracted_links_t structure containing all found links,
 *         or NULL on failure. The caller must free the result with ws_free_extracted_links().
 */
extracted_links_t *ws_extract_links(const char *html_content, size_t html_len, 
                                    const char *content_type, const char *base_url);

/**
 * @brief Free an extracted_links_t structure and all its internal memory.
 *
 * This function releases all memory allocated for the links and the structure itself.
 *
 * @param links_data Pointer to the extracted_links_t structure to free.
 */
void ws_free_extracted_links(extracted_links_t *links_data);

/**
 * @brief Cleanup resources used by the extraction module.
 *
 * This function should be called when extraction operations are finished,
 * to release any global resources allocated by ws_extract_init().
 */
void ws_extract_cleanup(void);

#endif // __WS_EXTRACT_H__