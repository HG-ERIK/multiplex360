#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include "Http.h"
#include "Tls.h"
#include "Log.h"

namespace
{
    // Socket option used by 360 homebrew to allow plain (non-XSP) TCP to PCs and the internet.
    const int SO_XBOX_INSECURE = 0x5801;

    std::string FormatIp( const IN_ADDR& a )
    {
        char buf[32];
        sprintf_s( buf, "%u.%u.%u.%u", a.S_un.S_un_b.s_b1, a.S_un.S_un_b.s_b2,
                   a.S_un.S_un_b.s_b3, a.S_un.S_un_b.s_b4 );
        return buf;
    }

    std::string ToLower( std::string s )
    {
        for( size_t i = 0; i < s.size(); ++i )
            if( s[i] >= 'A' && s[i] <= 'Z' )
                s[i] = (char)( s[i] - 'A' + 'a' );
        return s;
    }

    // Decodes a "Transfer-Encoding: chunked" body.
    std::string Dechunk( const std::string& in )
    {
        std::string out;
        size_t pos = 0;
        while( pos < in.size() )
        {
            size_t eol = in.find( "\r\n", pos );
            if( eol == std::string::npos )
                break;
            unsigned long len = strtoul( in.substr( pos, eol - pos ).c_str(), NULL, 16 );
            pos = eol + 2;
            if( len == 0 || len > in.size() - pos )
                break;
            out.append( in, pos, len );
            pos += len + 2;
        }
        return out;
    }

    // DNS answers are cached (plex.direct names are looked up for every poster otherwise) and
    // lookups run one at a time: XNetDnsLookup from several threads at once is not safe.
    CRITICAL_SECTION                      g_dnsLock;
    LONG                                  g_dnsInit = 0;
    std::map<std::string, IN_ADDR>*       g_dnsCache = NULL;

    void DnsLockInit()
    {
        if( InterlockedCompareExchange( &g_dnsInit, 1, 0 ) == 0 )
        {
            InitializeCriticalSection( &g_dnsLock );
            g_dnsCache = new std::map<std::string, IN_ADDR>;
            InterlockedExchange( &g_dnsInit, 2 );
        }
        while( g_dnsInit != 2 )
            Sleep( 0 );
    }

    bool Resolve( const std::string& host, IN_ADDR& addr, std::string& error )
    {
        addr.s_addr = inet_addr( host.c_str() );
        if( addr.s_addr != INADDR_NONE )
            return true;

        DnsLockInit();
        EnterCriticalSection( &g_dnsLock );
        std::map<std::string, IN_ADDR>::iterator cached = g_dnsCache->find( host );
        if( cached != g_dnsCache->end() )
        {
            addr = cached->second;
            LeaveCriticalSection( &g_dnsLock );
            return true;
        }

        XNDNS* dns = NULL;
        INT err = XNetDnsLookup( host.c_str(), NULL, &dns );
        if( err != 0 || !dns )
        {
            error = "DNS lookup could not start for " + host;
            LeaveCriticalSection( &g_dnsLock );
            return false;
        }
        for( int i = 0; i < 100 && dns->iStatus == WSAEINPROGRESS; ++i )
            Sleep( 50 );

        bool ok = dns->iStatus == 0 && dns->cina > 0;
        if( ok )
        {
            addr = dns->aina[0];
            ( *g_dnsCache )[host] = addr;
        }
        else
            error = "Cannot find " + host + " (DNS)";
        XNetDnsRelease( dns );
        LeaveCriticalSection( &g_dnsLock );
        return ok;
    }

    // Connects with a timeout (a blocking connect to a dead address can hang for ~20 s).
    SOCKET Connect( const IN_ADDR& addr, int port, DWORD timeoutMs, std::string& error )
    {
        SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
        if( s == INVALID_SOCKET )
        {
            error = "socket() failed";
            return INVALID_SOCKET;
        }

        BOOL yes = TRUE;
        setsockopt( s, SOL_SOCKET, SO_XBOX_INSECURE, (const char*)&yes, sizeof( yes ) );
        setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof( timeoutMs ) );
        setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof( timeoutMs ) );

        u_long nonBlocking = 1;
        ioctlsocket( s, FIONBIO, &nonBlocking );

        sockaddr_in sa;
        ZeroMemory( &sa, sizeof( sa ) );
        sa.sin_family = AF_INET;
        sa.sin_port = htons( (u_short)port );
        sa.sin_addr = addr;

        char target[48];
        sprintf_s( target, "%s:%d", FormatIp( addr ).c_str(), port );

        if( connect( s, (sockaddr*)&sa, sizeof( sa ) ) != 0 && WSAGetLastError() != WSAEWOULDBLOCK )
        {
            char buf[96];
            sprintf_s( buf, "Cannot connect to %s (error %d)", target, WSAGetLastError() );
            error = buf;
            closesocket( s );
            return INVALID_SOCKET;
        }

        fd_set writable, failed;
        FD_ZERO( &writable );
        FD_ZERO( &failed );
        FD_SET( s, &writable );
        FD_SET( s, &failed );
        timeval tv = { (long)( timeoutMs / 1000 ), (long)( ( timeoutMs % 1000 ) * 1000 ) };
        int n = select( 0, NULL, &writable, &failed, &tv );
        if( n <= 0 || FD_ISSET( s, &failed ) )
        {
            error = std::string( n == 0 ? "Timed out connecting to " : "Connection refused by " ) + target;
            closesocket( s );
            return INVALID_SOCKET;
        }

        nonBlocking = 0;
        ioctlsocket( s, FIONBIO, &nonBlocking );
        return s;
    }

    // Plain or TLS byte stream over a connected socket.
    struct Pipe
    {
        SOCKET           sock;
        Tls::Connection* tls;

        int Send( const char* p, int n ) { return tls ? tls->Send( p, n ) : send( sock, p, n, 0 ); }
        int Recv( char* p, int n )       { return tls ? tls->Recv( p, n ) : recv( sock, p, n, 0 ); }
    };
}

bool Http::Url::Parse( const std::string& url )
{
    size_t rest;
    if( url.compare( 0, 8, "https://" ) == 0 )      { https = true;  port = 443; rest = 8; }
    else if( url.compare( 0, 7, "http://" ) == 0 )  { https = false; port = 80;  rest = 7; }
    else return false;

    size_t slash = url.find( '/', rest );
    std::string hostPort = url.substr( rest, slash == std::string::npos ? std::string::npos : slash - rest );
    path = slash == std::string::npos ? "/" : url.substr( slash );

    size_t colon = hostPort.find( ':' );
    host = hostPort.substr( 0, colon );
    if( colon != std::string::npos )
        port = atoi( hostPort.c_str() + colon + 1 );
    return !host.empty() && port > 0;
}

std::string Http::Escape( const std::string& s )
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for( size_t i = 0; i < s.size(); ++i )
    {
        unsigned char c = (unsigned char)s[i];
        if( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ||
            c == '-' || c == '_' || c == '.' || c == '~' )
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool Http::Startup( std::string& error, std::string& localIp )
{
    XNetStartupParams xnsp;
    ZeroMemory( &xnsp, sizeof( xnsp ) );
    xnsp.cfgSizeOfStruct = sizeof( xnsp );
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    INT err = XNetStartup( &xnsp );
    if( err != 0 )
    {
        char buf[64];
        sprintf_s( buf, "XNetStartup failed (%d)", err );
        error = buf;
        return false;
    }

    WSADATA wsa;
    err = WSAStartup( MAKEWORD( 2, 2 ), &wsa );
    if( err != 0 )
    {
        char buf[64];
        sprintf_s( buf, "WSAStartup failed (%d)", err );
        error = buf;
        return false;
    }

    // Wait up to 15 s for the cable link and a DHCP address.
    XNADDR addr;
    DWORD status = XNET_GET_XNADDR_PENDING;
    for( int i = 0; i < 150; ++i )
    {
        status = XNetGetTitleXnAddr( &addr );
        if( status != XNET_GET_XNADDR_PENDING )
            break;
        Sleep( 100 );
    }

    if( status == XNET_GET_XNADDR_PENDING || ( status & XNET_GET_XNADDR_NONE ) )
    {
        DWORD link = XNetGetEthernetLinkStatus();
        error = ( link & XNET_ETHERNET_LINK_ACTIVE ) ? "No IP address from DHCP"
                                                     : "Network cable not connected";
        return false;
    }

    localIp = FormatIp( addr.ina );
    Log::Write( "Network up, console IP %s", localIp.c_str() );
    return Tls::Startup( error );
}

Http::Response Http::Request( const char* method, const std::string& urlStr, const std::string& extraHeaders,
                              const std::string& body, unsigned long timeoutMs )
{
    Response r;
    Url url;
    if( !url.Parse( urlStr ) )
    {
        r.error = "Bad URL: " + urlStr;
        return r;
    }

    IN_ADDR addr;
    if( !Resolve( url.host, addr, r.error ) )
        return r;

    SOCKET s = Connect( addr, url.port, timeoutMs, r.error );
    if( s == INVALID_SOCKET )
        return r;

    Tls::Connection tls;
    Pipe stream = { s, NULL };
    if( url.https )
    {
        if( !tls.Handshake( (unsigned int)s, url.host, r.error ) )
        {
            closesocket( s );
            return r;
        }
        stream.tls = &tls;
    }

    // Host gets the port only when it isn't the scheme's default.
    std::string hostHeader = url.host;
    if( url.port != ( url.https ? 443 : 80 ) )
    {
        char portStr[16];
        sprintf_s( portStr, ":%d", url.port );
        hostHeader += portStr;
    }
    char lenStr[64];
    sprintf_s( lenStr, "Content-Length: %u\r\n", (unsigned)body.size() );

    // HTTP/1.1 (plex.tv answers 1.0 requests with 426) with Connection: close, so the
    // reply still ends when the server closes the socket.
    std::string req = std::string( method ) + " " + url.path + " HTTP/1.1\r\n"
                      "Host: " + hostHeader + "\r\n"
                      "Accept: application/xml\r\n"
                      "Connection: close\r\n" +
                      extraHeaders;
    if( !body.empty() || strcmp( method, "POST" ) == 0 )
        req += lenStr;
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while( sent < req.size() )
    {
        int n = stream.Send( req.c_str() + sent, (int)( req.size() - sent ) );
        if( n <= 0 )
        {
            r.error = "Sending the request failed";
            tls.Close();
            closesocket( s );
            return r;
        }
        sent += n;
    }

    std::string raw;
    char buf[8192];
    for( ;; )
    {
        int n = stream.Recv( buf, sizeof( buf ) );
        if( n <= 0 )
        {
            if( n < 0 && raw.empty() )
                Log::Write( "Recv from %s failed: %d", url.host.c_str(), n );
            break;
        }
        raw.append( buf, n );
    }
    tls.Close();
    closesocket( s );

    size_t headerEnd = raw.find( "\r\n\r\n" );
    if( raw.compare( 0, 5, "HTTP/" ) != 0 || headerEnd == std::string::npos )
    {
        r.error = raw.empty() ? "Server closed the connection without replying"
                              : "Reply was not HTTP";
        return r;
    }

    size_t sp = raw.find( ' ' );
    r.status = atoi( raw.c_str() + sp + 1 );

    std::string headers = ToLower( raw.substr( 0, headerEnd ) );
    r.body = raw.substr( headerEnd + 4 );
    if( headers.find( "transfer-encoding: chunked" ) != std::string::npos )
        r.body = Dechunk( r.body );

    // Log the URL without its query: Plex queries can carry tokens.
    Log::Write( "%s %s://%s%s -> %d (%u bytes)", method, url.https ? "https" : "http", url.host.c_str(),
                url.path.substr( 0, url.path.find( '?' ) ).c_str(), r.status, (unsigned)r.body.size() );
    return r;
}


//--------------------------------------------------------------------------------------
// Http::Stream
//--------------------------------------------------------------------------------------
Http::Stream::Stream()
    : m_socket( (unsigned int)INVALID_SOCKET ), m_tls( NULL ), m_pendingPos( 0 ), m_status( 0 ),
      m_totalSize( -1 ), m_remaining( -1 ), m_chunked( false ), m_chunkLeft( 0 ), m_eof( false )
{
}

Http::Stream::~Stream()
{
    Close();
}

void Http::Stream::Close()
{
    if( m_tls )
    {
        Tls::Connection* tls = (Tls::Connection*)m_tls;
        tls->Close();
        delete tls;
        m_tls = NULL;
    }
    if( m_socket != (unsigned int)INVALID_SOCKET )
    {
        closesocket( (SOCKET)m_socket );
        m_socket = (unsigned int)INVALID_SOCKET;
    }
    m_pending.clear();
    m_pendingPos = 0;
}

int Http::Stream::RawRead( char* buf, int size )
{
    if( m_pendingPos < m_pending.size() )
    {
        int n = (int)min( (size_t)size, m_pending.size() - m_pendingPos );
        memcpy( buf, m_pending.data() + m_pendingPos, n );
        m_pendingPos += n;
        if( m_pendingPos == m_pending.size() )
        {
            m_pending.clear();
            m_pendingPos = 0;
        }
        return n;
    }
    if( m_tls )
        return ( (Tls::Connection*)m_tls )->Recv( buf, size );
    return recv( (SOCKET)m_socket, buf, size, 0 );
}

bool Http::Stream::ReadLine( std::string& line )
{
    line.clear();
    char c;
    while( RawRead( &c, 1 ) == 1 )
    {
        if( c == '\n' )
        {
            if( !line.empty() && line[line.size() - 1] == '\r' )
                line.erase( line.size() - 1 );
            return true;
        }
        line += c;
        if( line.size() > 8192 )
            return false;
    }
    return false;
}

bool Http::Stream::Open( const std::string& urlStr, const std::string& extraHeaders, __int64 offset,
                         std::string& error )
{
    Close();
    m_status = 0;
    m_totalSize = m_remaining = -1;
    m_chunked = false;
    m_chunkLeft = 0;
    m_eof = false;

    Url url;
    if( !url.Parse( urlStr ) )
    {
        error = "Bad URL";
        return false;
    }
    IN_ADDR addr;
    if( !Resolve( url.host, addr, error ) )
        return false;
    SOCKET s = Connect( addr, url.port, 15000, error );
    if( s == INVALID_SOCKET )
        return false;
    m_socket = (unsigned int)s;

    if( url.https )
    {
        Tls::Connection* tls = new Tls::Connection;
        m_tls = tls;
        if( !tls->Handshake( (unsigned int)s, url.host, error ) )
        {
            Close();
            return false;
        }
    }

    std::string hostHeader = url.host;
    if( url.port != ( url.https ? 443 : 80 ) )
    {
        char portStr[16];
        sprintf_s( portStr, ":%d", url.port );
        hostHeader += portStr;
    }
    std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + hostHeader + "\r\nConnection: close\r\n" +
                      extraHeaders;
    if( offset > 0 )
    {
        char range[64];
        sprintf_s( range, "Range: bytes=%I64d-\r\n", offset );
        req += range;
    }
    req += "\r\n";

    size_t sent = 0;
    while( sent < req.size() )
    {
        int n = m_tls ? ( (Tls::Connection*)m_tls )->Send( req.c_str() + sent, (int)( req.size() - sent ) )
                      : send( s, req.c_str() + sent, (int)( req.size() - sent ), 0 );
        if( n <= 0 )
        {
            error = "Sending the request failed";
            Close();
            return false;
        }
        sent += n;
    }

    std::string line;
    if( !ReadLine( line ) || line.compare( 0, 5, "HTTP/" ) != 0 )
    {
        error = "No HTTP reply";
        Close();
        return false;
    }
    m_status = atoi( line.c_str() + line.find( ' ' ) + 1 );

    __int64 contentLength = -1;
    while( ReadLine( line ) && !line.empty() )
    {
        std::string lower = ToLower( line );
        if( lower.compare( 0, 15, "content-length:" ) == 0 )
            contentLength = _atoi64( line.c_str() + 15 );
        else if( lower.compare( 0, 14, "content-range:" ) == 0 )
        {
            size_t slash = line.find( '/' );
            if( slash != std::string::npos && line[slash + 1] != '*' )
                m_totalSize = _atoi64( line.c_str() + slash + 1 );
        }
        else if( lower.compare( 0, 18, "transfer-encoding:" ) == 0 &&
                 lower.find( "chunked" ) != std::string::npos )
            m_chunked = true;
    }

    if( m_status != 200 && m_status != 206 )
    {
        char buf[48];
        sprintf_s( buf, "Server replied HTTP %d", m_status );
        error = buf;
        char body[300];
        int n = RawRead( body, sizeof( body ) - 1 );
        body[n > 0 ? n : 0] = 0;
        for( int i = 0; body[i]; ++i )
            if( body[i] == '\r' || body[i] == '\n' )
                body[i] = ' ';
        Log::Write( "Stream %s%s -> %d: %s", url.host.c_str(), url.path.substr( 0, url.path.find( '?' ) ).c_str(),
                    m_status, body );
        Close();
        return false;
    }
    m_remaining = m_chunked ? -1 : contentLength;
    if( m_totalSize < 0 && m_status == 200 )
        m_totalSize = contentLength;

    Log::Write( "Stream %s%s at %I64d -> %d (size %I64d%s)", url.host.c_str(),
                url.path.substr( 0, url.path.find( '?' ) ).c_str(), offset, m_status, m_totalSize,
                m_chunked ? ", chunked" : "" );
    return true;
}

int Http::Stream::Read( char* buf, int size )
{
    if( m_eof || m_socket == (unsigned int)INVALID_SOCKET )
        return 0;

    if( m_chunked )
    {
        if( m_chunkLeft == 0 )
        {
            std::string line;
            if( !ReadLine( line ) )
                return -1;
            if( line.empty() && !ReadLine( line ) )   // CRLF after the previous chunk
                return -1;
            m_chunkLeft = (__int64)strtoul( line.c_str(), NULL, 16 );
            if( m_chunkLeft == 0 )
            {
                m_eof = true;
                return 0;
            }
        }
        int want = (int)min( (__int64)size, m_chunkLeft );
        int n = RawRead( buf, want );
        if( n > 0 )
            m_chunkLeft -= n;
        return n;
    }

    if( m_remaining == 0 )
    {
        m_eof = true;
        return 0;
    }
    int want = m_remaining > 0 ? (int)min( (__int64)size, m_remaining ) : size;
    int n = RawRead( buf, want );
    if( n > 0 && m_remaining > 0 )
        m_remaining -= n;
    if( n == 0 )
        m_eof = true;
    return n;
}

void Http::Stream::Abort()
{
    // shutdown() wakes a recv() blocked on this socket; Close() then runs on the owner thread.
    SOCKET s = (SOCKET)m_socket;
    if( s != INVALID_SOCKET )
        shutdown( s, SD_BOTH );
}
