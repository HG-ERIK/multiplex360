// mbedTLS 2.16 configuration for the Xbox 360: TLS 1.2 client only.
// Selected with MBEDTLS_CONFIG_FILE="mbedtls_config_xbox.h".
#ifndef MBEDTLS_CONFIG_XBOX_H
#define MBEDTLS_CONFIG_XBOX_H

// --- Platform -------------------------------------------------------------------------
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE          // certificate validity dates
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY     // no CryptGenRandom / /dev/urandom on the 360
#define MBEDTLS_ENTROPY_HARDWARE_ALT    // mbedtls_hardware_poll() in Tls.cpp uses XNetRandom
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// --- TLS ------------------------------------------------------------------------------
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_SESSION_TICKETS

// ECDHE only: forward secrecy, no static-RSA key exchange.
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

// --- Ciphers --------------------------------------------------------------------------
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

// --- Hashes ---------------------------------------------------------------------------
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

// --- Public key -----------------------------------------------------------------------
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

// --- Certificates ---------------------------------------------------------------------
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

// --- Diagnostics ----------------------------------------------------------------------
#define MBEDTLS_ERROR_C                 // mbedtls_strerror() for readable errors

#include "mbedtls/check_config.h"

#endif
