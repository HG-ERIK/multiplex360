#pragma once
#include <string>

namespace Http
{
    struct Response
    {
        int         status;   // HTTP status code, 0 if the request never got a reply
        std::string body;
        std::string error;    // set when status == 0

        Response() : status( 0 ) {}
    };

    struct Url
    {
        bool        https;
        std::string host;
        int         port;
        std::string path;     // includes the query string; starts with '/'

        Url() : https( false ), port( 80 ) {}
        bool Parse( const std::string& url );
    };

    // Starts the network stack, waits for DHCP, and sets up TLS. Call once.
    bool Startup( std::string& error, std::string& localIp );

    // HTTP/1.1 request (Connection: close) over http:// or https://. extraHeaders is "Name: value\r\n" lines.
    Response Request( const char* method, const std::string& url, const std::string& extraHeaders,
                      const std::string& body = std::string(), unsigned long timeoutMs = 10000 );

    inline Response Get( const std::string& url, const std::string& extraHeaders, unsigned long timeoutMs = 10000 )
    {
        return Request( "GET", url, extraHeaders, std::string(), timeoutMs );
    }

    // Percent-encodes a query parameter value.
    std::string Escape( const std::string& s );

    // A GET whose body is read incrementally (video files), starting at a byte offset.
    // Handles http/https, Content-Length and chunked bodies.
    class Stream
    {
    public:
        Stream();
        ~Stream();

        bool Open( const std::string& url, const std::string& extraHeaders, __int64 offset,
                   std::string& error );
        int  Read( char* buf, int size );    // bytes read, 0 at end, < 0 on error
        void Close();

        // From another thread: break the connection so a blocked Read() returns at once.
        void Abort();

        int     Status() const     { return m_status; }
        __int64 TotalSize() const  { return m_totalSize; }   // -1 if unknown (live transcode)

    private:
        int  RawRead( char* buf, int size );
        bool ReadLine( std::string& line );

        unsigned int     m_socket;
        void*            m_tls;          // Tls::Connection*
        std::string      m_pending;      // bytes read past the headers
        size_t           m_pendingPos;
        int              m_status;
        __int64          m_totalSize;
        __int64          m_remaining;    // body bytes left (Content-Length), -1 if unknown
        bool             m_chunked;
        __int64          m_chunkLeft;    // bytes left in the current chunk
        bool             m_eof;

        Stream( const Stream& );
        Stream& operator=( const Stream& );
    };
}
