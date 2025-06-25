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
#ifndef __WS_SSL_H__
#define __WS_SSL_H__

// OpenSSL Headers
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h> // For CONF_modules_free
#include <openssl/evp.h>  // For EVP_cleanup

/**
 * @brief Initializes the OpenSSL library.
 * This function performs global initialization of the OpenSSL library, including
 * loading error strings and algorithms.
 * It must be called before using any OpenSSL functionalities.
 * @return 0 on success, -1 on failure.
 */
int ws_ssl_init_libs(void);

/**
 * @brief Cleans up OpenSSL library resources.
 * This function releases all global resources used by the OpenSSL library.
 * Call this function before application exit to prevent memory leaks.
 */
void ws_ssl_cleanup_libs(void);

/**
 * @brief Creates a new SSL context for client connections.
 * This SSL context (SSL_CTX) is used to configure SSL/TLS protocol versions,
 * certificate verification settings, etc.
 * Typically, an application only needs one client SSL_CTX instance to reuse.
 * @return A pointer to the newly created SSL_CTX structure, or NULL on failure.
 */
SSL_CTX *ws_ssl_client_ctx_new(void);

/**
 * @brief Frees an SSL context.
 * @param ctx A pointer to the SSL_CTX structure to be freed.
 */
void ws_ssl_free_ctx(SSL_CTX *ctx);

/**
 * @brief Creates an SSL object for a new HTTPS connection.
 * This function creates an SSL object from the given SSL_CTX and configures
 * the hostname for SNI (Server Name Indication).
 * The `evhttp_connection_base_ssl_new` function from libevent will take over
 * the lifecycle management of this SSL object.
 * @param ctx The SSL_CTX used to create the SSL object.
 * @param hostname The hostname of the target server, used for SNI.
 * @return A pointer to the newly created SSL structure, or NULL on failure.
 */
SSL *ws_ssl_new_connection_ssl(SSL_CTX *ctx, const char *hostname);

#endif // __WS_SSL_H__