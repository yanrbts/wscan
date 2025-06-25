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
#include <string.h>
#include <ws_ssl.h>
#include <ws_log.h>

/**
 * @brief Initializes the OpenSSL library.
 */
int ws_ssl_init_libs(void) {
    // Register all OpenSSL algorithms and load error strings
    // OpenSSL 1.1.0+ simplifies initialization; these functions are
    // old version macros or no longer strictly required.
    // For compatibility, retaining these calls is harmless.
    SSL_library_init(); // Or OPENSSL_init_ssl() for OpenSSL 1.1.0+
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms(); // Or EVP_add_all_algorithms() for OpenSSL 1.1.0+

    ws_log_info("OpenSSL libraries initialized.");
    return 0;
}

/**
 * @brief Cleans up OpenSSL library resources.
 */
void ws_ssl_cleanup_libs(void) {
    // Clean up OpenSSL resources
    EVP_cleanup();
    ERR_free_strings();
    // Conf_modules_free() and CRYPTO_cleanup_all_ex_data() are also part of cleanup
    // In OpenSSL 1.1.0+, most cleanup is handled by OPENSSL_cleanup()
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_state(0);
#endif
    ws_log_info("OpenSSL libraries cleaned up.");
}

/**
 * @brief Creates a new SSL context for client connections.
 */
SSL_CTX *ws_ssl_client_ctx_new(void) {
    // Use TLSv1.2/1.3 negotiation method, or TLS_client_method() for broader compatibility
    const SSL_METHOD *method = TLS_client_method(); // Recommended for OpenSSL 1.1.0+

    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        ws_log_error("Failed to create SSL_CTX: %s", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    // Disable insecure protocol versions (e.g., SSLv2, SSLv3)
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION); // Enforce minimum TLS 1.2
    // If client certificate verification of the server is required, uncomment and configure:
    // SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    // SSL_CTX_load_verify_locations(ctx, "path/to/ca-certificates.pem", NULL);

    ws_log_info("SSL_CTX created for client connections.");
    return ctx;
}

/**
 * @brief Frees an SSL context.
 */
void ws_ssl_free_ctx(SSL_CTX *ctx) {
    if (ctx) {
        SSL_CTX_free(ctx);
        ws_log_info("SSL_CTX freed.");
    }
}

/**
 * @brief Creates an SSL object for a new HTTPS connection.
 */
SSL *ws_ssl_new_connection_ssl(SSL_CTX *ctx, const char *hostname) {
    if (!ctx) {
        ws_log_error("SSL_CTX is NULL when creating new SSL object.");
        return NULL;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        ws_log_error("Failed to create SSL object: %s", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    // Set SNI (Server Name Indication)
    // This is necessary for hosting multiple HTTPS sites on the same IP address.
    if (hostname && strlen(hostname) > 0) {
        if (!SSL_set_tlsext_host_name(ssl, hostname)) {
            ws_log_error("Failed to set SNI hostname '%s': %s", hostname, 
                        ERR_error_string(ERR_get_error(), NULL));
            SSL_free(ssl);
            return NULL;
        }
        ws_log_info("SNI hostname set to: %s", hostname);
    }

    // By default, OpenSSL clients verify the server certificate.
    // If stricter verification is needed, it can be configured in SSL_CTX
    // or set manually here using SSL_set_verify().

    ws_log_info("New SSL object created for connection to %s.", hostname ? hostname : "unknown host");
    return ssl;
}