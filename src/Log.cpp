#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdarg.h>
#include "Log.h"

namespace
{
    char             g_path[MAX_PATH] = "";
    CRITICAL_SECTION g_lock;
    SOCKET           g_udp = INVALID_SOCKET;
    sockaddr_in      g_remote;
}

void Log::Open( const char* path )
{
    InitializeCriticalSection( &g_lock );
    strcpy_s( g_path, path );
    FILE* f = fopen( g_path, "w" );   // truncate the previous run's log
    if( f )
        fclose( f );
}

void Log::Write( const char* fmt, ... )
{
    char msg[1024];
    va_list args;
    va_start( args, fmt );
    vsnprintf_s( msg, sizeof( msg ), _TRUNCATE, fmt, args );
    va_end( args );

    char line[1100];
    sprintf_s( line, "[%8.3f] %s\n", GetTickCount() / 1000.0, msg );

    OutputDebugStringA( line );
    if( g_udp != INVALID_SOCKET )
        sendto( g_udp, line, (int)strlen( line ), 0, (sockaddr*)&g_remote, sizeof( g_remote ) );
    if( g_path[0] )
    {
        // Reopened per line so the file isn't held open and xbcp can pull it while running.
        EnterCriticalSection( &g_lock );
        FILE* f = fopen( g_path, "a" );
        if( f )
        {
            fputs( line, f );
            fclose( f );
        }
        LeaveCriticalSection( &g_lock );
    }
}

void Log::SetRemote( const char* ip, int port )
{
    SOCKET s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if( s == INVALID_SOCKET )
        return;
    BOOL yes = TRUE;
    setsockopt( s, SOL_SOCKET, 0x5801, (const char*)&yes, sizeof( yes ) );   // allow plain UDP to the PC
    ZeroMemory( &g_remote, sizeof( g_remote ) );
    g_remote.sin_family = AF_INET;
    g_remote.sin_port = htons( (u_short)port );
    g_remote.sin_addr.s_addr = inet_addr( ip );
    g_udp = s;
    Write( "Remote log to %s:%d", ip, port );
}