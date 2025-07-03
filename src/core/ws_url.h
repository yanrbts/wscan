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
#ifndef __WS_URL_H__
#define __WS_URL_H__

#include <stddef.h> // For size_t

// 函数声明
/**
 * @brief 从完整的 URL 中提取 "一级域名" (FLDs)。
 * @param url 要解析的 URL 字符串。
 * @return 提取到的一级域名的新分配字符串。如果无法解析或发生错误，返回 NULL。
 * 调用者必须负责释放返回的字符串。
 * @note 这是一个非常简化的实现，不使用公共后缀列表，可能不完全准确。
 * 例如，对于 "www.google.co.uk"，它可能返回 "co.uk" 或 "google.co.uk" 取决于实现。
 * 此版本尝试返回 "domain.tld" 格式。
 */
char* ws_url_get_fld(const char *url);

/**
 * @brief 从完整的 URL 中提取主机名。
 * @param url 要解析的 URL 字符串。
 * @return 提取到的主机名的新分配字符串。如果无法解析或发生错误，返回 NULL。
 * 调用者必须负责释放返回的字符串。
 */
char* ws_url_get_hostname(const char *url);

/**
 * @brief 尝试将相对 URL 转换为绝对 URL。
 * @param base_url 基础 URL，用于解析相对 URL。
 * @param relative_url 相对 URL 字符串。
 * @return 转换后的绝对 URL 的新分配字符串。如果无法解析或发生错误，返回 NULL。
 * 调用者必须负责释放返回的字符串。
 * @note 这是一个非常简化的实现，不支持复杂的路径解析或`..`等。
 */
char* ws_url_resolve(const char *base_url, const char *relative_url);

#endif // WS_URL_H