#include <xtl.h>
#include <stdio.h>
#include <vector>
#include "Updater.h"
#include "Http.h"
#include "Font.h"
#include "Draw.h"
#include "App.h"
#include "Guard.h"
#include "Log.h"

namespace
{
    const char* const RELEASES = "https://api.github.com/repos/HG-ERIK/multiplex360/releases/latest";
    const char* const HEADERS = "User-Agent: Multiplex360\r\nAccept: application/vnd.github+json\r\n";
    const char* const NEW_FILE = "game:\\default.new";
    const char* const OLD_FILE = "game:\\default.old";
    const char* const XEX_FILE = "game:\\default.xex";

    enum State { IDLE, ASK, DOWNLOADING, FAILED, DONE };
    volatile LONG g_state = IDLE;
    volatile LONG g_got = 0, g_total = 0;      // download progress (bytes)
    std::string   g_version, g_url, g_error, g_from = RELEASES;
    std::wstring  g_notes;

    // "0.10" > "0.9": compares dot-separated numbers.
    bool Newer( const std::string& a, const std::string& b )
    {
        const char *p = a.c_str(), *q = b.c_str();
        for( ;; )
        {
            long x = strtol( p, (char**)&p, 10 ), y = strtol( q, (char**)&q, 10 );
            if( x != y ) return x > y;
            if( *p != '.' && *q != '.' ) return false;
            if( *p == '.' ) ++p;
            if( *q == '.' ) ++q;
        }
    }

    // The string value of "key" in a JSON text (the first one after from), with its escapes decoded.
    std::wstring JsonString( const std::string& json, const char* key, size_t from = 0, size_t* end = NULL )
    {
        std::string k = std::string( "\"" ) + key + "\"";
        size_t at = json.find( k, from );
        if( at == std::string::npos ) return std::wstring();
        at = json.find( ':', at + k.size() );
        if( at == std::string::npos ) return std::wstring();
        at = json.find_first_not_of( " \t\r\n", at + 1 );
        if( at == std::string::npos || json[at] != '"' ) return std::wstring();
        std::wstring out;
        for( size_t i = at + 1; i < json.size(); ++i )
        {
            unsigned char c = json[i];
            if( c == '"' ) { if( end ) *end = i; break; }
            if( c == '\\' && i + 1 < json.size() )
            {
                char e = json[++i];
                if( e == 'n' ) out += L'\n';
                else if( e == 'r' || e == 't' ) out += L' ';
                else if( e == 'u' && i + 4 < json.size() ) { out += (wchar_t)strtol( json.substr( i + 1, 4 ).c_str(), NULL, 16 ); i += 4; }
                else out += (wchar_t)e;
                continue;
            }
            // UTF-8 to UTF-16 (2- and 3-byte forms are enough for release notes).
            if( c >= 0xE0 && i + 2 < json.size() ) { out += (wchar_t)( ( ( c & 15 ) << 12 ) | ( ( json[i + 1] & 63 ) << 6 ) | ( json[i + 2] & 63 ) ); i += 2; }
            else if( c >= 0xC0 && i + 1 < json.size() ) { out += (wchar_t)( ( ( c & 31 ) << 6 ) | ( json[i + 1] & 63 ) ); ++i; }
            else out += (wchar_t)c;
        }
        return out;
    }

    std::string Narrow( const std::wstring& w )
    {
        std::string s;
        for( size_t i = 0; i < w.size(); ++i ) s += w[i] < 128 ? (char)w[i] : '?';
        return s;
    }

    DWORD WINAPI CheckThread( LPVOID )
    {
        Http::Response r = Http::Get( g_from, HEADERS, 15000 );
        if( r.status != 200 )
        {
            Log::Write( "Update check: HTTP %d %s", r.status, r.error.c_str() );
            return 0;
        }
        std::string tag = Narrow( JsonString( r.body, "tag_name" ) );
        std::string version = tag.size() && ( tag[0] == 'v' || tag[0] == 'V' ) ? tag.substr( 1 ) : tag;
        // The release's default.xex (uploaded next to the zip).
        std::string url;
        for( size_t at = 0; ( at = r.body.find( "\"browser_download_url\"", at ) ) != std::string::npos; ++at )
        {
            std::string u = Narrow( JsonString( r.body, "browser_download_url", at ) );
            if( u.size() > 12 && u.compare( u.size() - 12, 12, "/default.xex" ) == 0 ) { url = u; break; }
        }
        Log::Write( "Update check: newest %s (this is %s)%s", version.c_str(), MULTIPLEX_VERSION, url.empty() ? ", no default.xex in it" : "" );
        if( version.empty() || url.empty() || !Newer( version, MULTIPLEX_VERSION ) )
            return 0;
        g_version = version;
        g_url = url;
        g_notes = JsonString( r.body, "body" );
        InterlockedExchange( &g_state, ASK );
        return 0;
    }

    void Fail( const char* why )
    {
        g_error = why;
        Log::Write( "Update failed: %s", why );
        DeleteFile( NEW_FILE );
        InterlockedExchange( &g_state, FAILED );
    }

    DWORD WINAPI DownloadThread( LPVOID )
    {
        Http::Stream s;
        std::string error;
        if( !s.Open( g_url, "User-Agent: Multiplex360\r\n", 0, error ) ) { Fail( error.c_str() ); return 0; }
        g_total = (LONG)s.TotalSize();
        HANDLE f = CreateFile( NEW_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if( f == INVALID_HANDLE_VALUE ) { Fail( "can't write next to default.xex" ); return 0; }
        static char buf[64 * 1024];
        bool ok = true, first = true;
        for( ;; )
        {
            int n = s.Read( buf, sizeof( buf ) );
            if( n < 0 ) { ok = false; break; }
            if( n == 0 ) break;
            if( first && ( n < 4 || memcmp( buf, "XEX2", 4 ) ) ) { ok = false; break; }   // not an xex
            first = false;
            DWORD w;
            if( !WriteFile( f, buf, n, &w, NULL ) || w != (DWORD)n ) { ok = false; break; }
            InterlockedExchangeAdd( &g_got, n );
        }
        CloseHandle( f );
        if( !ok || first || ( g_total > 0 && g_got != g_total ) ) { Fail( "the download was cut off or not an xex" ); return 0; }
        // Swap: the running one becomes default.old (kept as a backup), the new one default.xex.
        DeleteFile( OLD_FILE );
        if( !MoveFile( XEX_FILE, OLD_FILE ) ) { Fail( "can't move the running default.xex aside" ); return 0; }
        if( !MoveFile( NEW_FILE, XEX_FILE ) )
        {
            MoveFile( OLD_FILE, XEX_FILE );                         // put the old one back
            Fail( "can't put the new default.xex in place" );
            return 0;
        }
        Log::Write( "Updated to %s (%d bytes), starting it", g_version.c_str(), (int)g_got );
        InterlockedExchange( &g_state, DONE );
        Sleep( 800 );                                               // let "Starting..." show
        XLaunchNewImage( XEX_FILE, 0 );
        return 0;
    }

    void Thread( LPTHREAD_START_ROUTINE fn )
    {
        HANDLE h = Guard_CreateThread( 64 * 1024, fn, NULL, 0 );
        if( h ) CloseHandle( h );
    }
}

void Updater::Start( const std::string& from )
{
    static bool started;
    if( started ) return;
    started = true;
    if( !from.empty() ) g_from = from;
    Thread( CheckThread );
}

bool Updater::Active()
{
    return g_state != IDLE;
}

void Updater::Input( WORD pressed )
{
    if( g_state == ASK )
    {
        if( pressed & XINPUT_GAMEPAD_A )
        {
            g_got = g_total = 0;
            InterlockedExchange( &g_state, DOWNLOADING );
            Thread( DownloadThread );
        }
        else if( pressed & ( XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK ) )
            InterlockedExchange( &g_state, IDLE );                  // later: asked again next start
    }
    else if( g_state == FAILED && ( pressed & ( XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B ) ) )
        InterlockedExchange( &g_state, IDLE );
}

void Updater::Render( Font& font, Font& title, float width )
{
    LONG state = g_state;
    if( state == IDLE ) return;
    const float w = (float)ScreenWidth();
    Draw::Rect( 0, 0, w, 720, 0xD8000000 );
    const float cx = width / 2;
    wchar_t line[160];
    title.Begin();
    swprintf_s( line, L"Multiplex 360 %S is out", g_version.c_str() );
    title.DrawText( cx, 70, 0xffe5a00d, line, FONT_CENTER_X );
    title.End();
    font.Begin();
    if( state == ASK )
    {
        swprintf_s( line, L"You have %S. What's new:", MULTIPLEX_VERSION );
        font.DrawText( cx, 130, 0xff8a8a8a, line, FONT_CENTER_X );
        // The release notes, line by line (long lines cut, at most 12).
        float y = 175;
        size_t at = 0;
        for( int n = 0; n < 12 && at < g_notes.size(); ++n )
        {
            size_t nl = g_notes.find( L'\n', at );
            std::wstring l = g_notes.substr( at, nl == std::wstring::npos ? std::wstring::npos : nl - at );
            at = nl == std::wstring::npos ? g_notes.size() : nl + 1;
            if( l.compare( 0, 2, L"- " ) == 0 || l.compare( 0, 2, L"* " ) == 0 ) l = L"\x2022 " + l.substr( 2 );
            while( !l.empty() && ( l[0] == L'#' ) ) l.erase( 0, 1 );
            font.DrawText( cx - width * 0.4f, y, 0xffeeeeee, l.c_str(), FONT_TRUNCATED, width * 0.8f );
            y += 30;
        }
        font.DrawText( cx, 560, 0xffeeeeee, L"A  Update now (your settings stay)          B  Later", FONT_CENTER_X );
    }
    else if( state == DOWNLOADING || state == DONE )
    {
        if( state == DONE )
            swprintf_s( line, L"Starting the new version..." );
        else if( g_total > 0 )
            swprintf_s( line, L"Downloading...  %d%%  (%.1f of %.1f MB)", (int)( 100.0 * g_got / g_total ), g_got / 1048576.0, g_total / 1048576.0 );
        else
            swprintf_s( line, L"Downloading...  %.1f MB", g_got / 1048576.0 );
        font.DrawText( cx, 330, 0xffeeeeee, line, FONT_CENTER_X );
    }
    else if( state == FAILED )
    {
        swprintf_s( line, L"The update didn't work: %S", g_error.c_str() );
        font.DrawText( cx, 330, 0xffe05050, line, FONT_CENTER_X );
        font.DrawText( cx, 380, 0xff8a8a8a, L"Nothing changed. Press A to go on.", FONT_CENTER_X );
    }
    font.End();
    if( state == DOWNLOADING && g_total > 0 )                      // the progress bar (outside the text)
    {
        float x0 = w / 2 - 250, x1 = w / 2 + 250;                   // Draw works in screen units, not the font window
        Draw::Rect( x0, 380, x1, 386, 0xff333333 );
        Draw::Rect( x0, 380, x0 + ( x1 - x0 ) * g_got / g_total, 386, 0xffe5a00d );
    }
}
