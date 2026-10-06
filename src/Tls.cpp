#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <time.h>
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"   // only for the MBEDTLS_ERR_NET_* codes
#include "Tls.h"
#include "Log.h"

#include "CaCerts.inc"

// Entropy for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT): the console's crypto RNG.
extern "C" int mbedtls_hardware_poll( void* data, unsigned char* output, size_t len, size_t* olen )
{
    (void)data;
    if( XNetRandom( output, (UINT)len ) != 0 )
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}

namespace
{
    mbedtls_entropy_context  g_entropy;
    mbedtls_ctr_drbg_context g_drbg;
    mbedtls_x509_crt         g_ca;
    bool                     g_ready = false;

    // Guards the shared entropy pool; each connection seeds its own DRBG from it.
    CRITICAL_SECTION         g_lock;

    struct Lock
    {
        Lock()  { EnterCriticalSection( &g_lock ); }
        ~Lock() { LeaveCriticalSection( &g_lock ); }
    };

    int SendCallback( void* ctx, const unsigned char* buf, size_t len )
    {
        SOCKET s = (SOCKET)(UINT_PTR)ctx;
        int n = send( s, (const char*)buf, (int)len, 0 );
        if( n >= 0 )
            return n;
        return WSAGetLastError() == WSAECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET
                                                  : MBEDTLS_ERR_NET_SEND_FAILED;
    }

    int RecvCallback( void* ctx, unsigned char* buf, size_t len )
    {
        SOCKET s = (SOCKET)(UINT_PTR)ctx;
        int n = recv( s, (char*)buf, (int)len, 0 );
        if( n >= 0 )
            return n;
        int err = WSAGetLastError();
        if( err == WSAECONNRESET )
            return MBEDTLS_ERR_NET_CONN_RESET;
        if( err == WSAETIMEDOUT )
            return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    // A console that never synced its clock would reject every certificate as
    // not-yet-valid or expired; only skip the date checks in that case.
    bool ClockLooksWrong()
    {
        time_t now = time( NULL );
        struct tm t;
        return gmtime_s( &t, &now ) != 0 || t.tm_year + 1900 < 2025;
    }
}

std::string Tls::ErrorString( int err )
{
    char buf[160];
    mbedtls_strerror( err, buf, sizeof( buf ) );
    char out[200];
    sprintf_s( out, "%s (-0x%04X)", buf, (unsigned)-err );
    return out;
}

bool Tls::Startup( std::string& error )
{
    if( g_ready )
        return true;

    InitializeCriticalSection( &g_lock );
    mbedtls_entropy_init( &g_entropy );
    mbedtls_ctr_drbg_init( &g_drbg );
    mbedtls_x509_crt_init( &g_ca );

    const char* pers = "xbox360-plex";
    int ret = mbedtls_ctr_drbg_seed( &g_drbg, mbedtls_entropy_func, &g_entropy,
                                     (const unsigned char*)pers, strlen( pers ) );
    if( ret != 0 )
    {
        error = "TLS RNG seed failed: " + ErrorString( ret );
        return false;
    }

    DWORD start = GetTickCount();
    ret = mbedtls_x509_crt_parse( &g_ca, (const unsigned char*)g_caPem, sizeof( g_caPem ) );
    if( ret < 0 )
    {
        error = "CA certificate parse failed: " + ErrorString( ret );
        return false;
    }
    Log::Write( "TLS ready (%d CA certs skipped, %u ms)%s", ret, GetTickCount() - start,
                ClockLooksWrong() ? " - console clock looks wrong, skipping cert dates" : "" );

    g_ready = true;
    return true;
}

struct Tls::Connection::Impl
{
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context drbg;
    bool                     open;
};

Tls::Connection::Connection() : m_impl( new Impl )
{
    mbedtls_ssl_init( &m_impl->ssl );
    mbedtls_ssl_config_init( &m_impl->conf );
    mbedtls_ctr_drbg_init( &m_impl->drbg );
    m_impl->open = false;
}

Tls::Connection::~Connection()
{
    Close();
    mbedtls_ssl_free( &m_impl->ssl );
    mbedtls_ssl_config_free( &m_impl->conf );
    mbedtls_ctr_drbg_free( &m_impl->drbg );
    delete m_impl;
}

bool Tls::Connection::Handshake( unsigned int socket, const std::string& host, std::string& error )
{
    if( !g_ready )
    {
        error = "TLS not initialized";
        return false;
    }

    int ret;
    {
        Lock lock;
        ret = mbedtls_ctr_drbg_seed( &m_impl->drbg, mbedtls_ctr_drbg_random, &g_drbg, NULL, 0 );
    }
    if( ret != 0 )
    {
        error = "TLS RNG seed failed: " + ErrorString( ret );
        return false;
    }

    mbedtls_ssl_config& conf = m_impl->conf;
    ret = mbedtls_ssl_config_defaults( &conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT );
    if( ret != 0 )
    {
        error = "TLS config failed: " + ErrorString( ret );
        return false;
    }

    // Verify manually below so a wrong console clock can be tolerated.
    mbedtls_ssl_conf_authmode( &conf, MBEDTLS_SSL_VERIFY_OPTIONAL );
    mbedtls_ssl_conf_ca_chain( &conf, &g_ca, NULL );
    mbedtls_ssl_conf_rng( &conf, mbedtls_ctr_drbg_random, &m_impl->drbg );
    mbedtls_ssl_conf_min_version( &conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3 );

    ret = mbedtls_ssl_setup( &m_impl->ssl, &conf );
    if( ret == 0 )
        ret = mbedtls_ssl_set_hostname( &m_impl->ssl, host.c_str() );
    if( ret != 0 )
    {
        error = "TLS setup failed: " + ErrorString( ret );
        return false;
    }

    mbedtls_ssl_set_bio( &m_impl->ssl, (void*)(UINT_PTR)socket, SendCallback, RecvCallback, NULL );

    DWORD start = GetTickCount();
    while( ( ret = mbedtls_ssl_handshake( &m_impl->ssl ) ) != 0 )
    {
        if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
        {
            error = "TLS handshake with " + host + " failed: " + ErrorString( ret );
            return false;
        }
    }

    uint32_t flags = mbedtls_ssl_get_verify_result( &m_impl->ssl );
    if( ClockLooksWrong() )
        flags &= ~( MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE );
    if( flags != 0 )
    {
        char info[256];
        mbedtls_x509_crt_verify_info( info, sizeof( info ), "", flags );
        error = "Certificate check failed for " + host + ": " + info;
        return false;
    }

    m_impl->open = true;
    Log::Write( "TLS %s %s with %s (%u ms)", mbedtls_ssl_get_version( &m_impl->ssl ),
                mbedtls_ssl_get_ciphersuite( &m_impl->ssl ), host.c_str(), GetTickCount() - start );
    return true;
}

int Tls::Connection::Send( const char* data, int len )
{
    int ret;
    do
        ret = mbedtls_ssl_write( &m_impl->ssl, (const unsigned char*)data, len );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE );
    return ret;
}

int Tls::Connection::Recv( char* data, int len )
{
    int ret;
    do
        ret = mbedtls_ssl_read( &m_impl->ssl, (unsigned char*)data, len );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    // Many servers just drop the connection after the reply instead of sending close_notify.
    if( ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_NET_CONN_RESET )
        return 0;
    return ret;
}

void Tls::Connection::Close()
{
    if( m_impl->open )
    {
        mbedtls_ssl_close_notify( &m_impl->ssl );
        m_impl->open = false;
    }
}
