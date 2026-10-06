#pragma once
#include <string>

// TLS 1.2 client over an already-connected socket (mbedTLS). Certificates are checked
// against the root CAs in CaCerts.inc and the hostname.
namespace Tls
{
    // One-time setup (RNG seeding, CA parsing). Call after the network is up.
    bool Startup( std::string& error );

    class Connection
    {
    public:
        Connection();
        ~Connection();

        bool Handshake( unsigned int socket, const std::string& host, std::string& error );
        int  Send( const char* data, int len );       // bytes sent, or < 0 on error
        int  Recv( char* data, int len );             // bytes read, 0 on clean close, < 0 on error
        void Close();

    private:
        struct Impl;
        Impl* m_impl;

        Connection( const Connection& );
        Connection& operator=( const Connection& );
    };

    std::string ErrorString( int mbedtlsError );
}
