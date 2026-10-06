#include "Guard.h"
#include "Log.h"
#include <stdlib.h>
#include <stdio.h>

namespace
{
    volatile LONG g_beat = 0;       // GetTickCount() of the last main-loop frame (0 = not started)
    volatile LONG g_leaving = 0;

    const DWORD STUCK_MS = 30000;

    void Leave( const char* why )
    {
        if( InterlockedExchange( &g_leaving, 1 ) )
        {
            for( ;; )
                Sleep( 1000 );      // another thread is already leaving
        }
        // The log may be what crashed; never let it stop us from leaving.
        __try
        {
            Log::Write( "GUARD: %s - leaving to the dashboard", why );
        }
        __except( EXCEPTION_EXECUTE_HANDLER )
        {
        }
        XLaunchNewImage( XLAUNCH_KEYWORD_DEFAULT_APP, 0 );
        for( ;; )
            Sleep( 1000 );
    }

    DWORD WINAPI Watchdog( LPVOID )
    {
        for( ;; )
        {
            Sleep( 2000 );
            LONG beat = g_beat;
            if( beat != 0 && GetTickCount() - (DWORD)beat > STUCK_MS )
                Leave( "main loop stuck for 30 s" );
        }
    }

    struct Start
    {
        LPTHREAD_START_ROUTINE fn;
        LPVOID                 arg;
    };

    DWORD WINAPI Trampoline( LPVOID p )
    {
        Start s = *(Start*)p;
        free( p );
        __try
        {
            return s.fn( s.arg );
        }
        __except( Guard_Filter( GetExceptionCode(), GetExceptionInformation(), "thread" ) )
        {
            return 0;
        }
    }
}

extern "C" void Guard_Start( void )
{
    HANDLE h = CreateThread( NULL, 16 * 1024, Watchdog, NULL, CREATE_SUSPENDED, NULL );
    XSetThreadProcessor( h, 1 );
    ResumeThread( h );
    CloseHandle( h );
}

extern "C" void Guard_Beat( void )
{
    InterlockedExchange( &g_beat, (LONG)GetTickCount() );
}

extern "C" int Guard_Filter( unsigned long code, EXCEPTION_POINTERS* info, const char* where )
{
    char why[128];
    void* address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : NULL;
    sprintf_s( why, "crash 0x%08lX at %p on %s thread %lu", code, address, where,
               GetCurrentThreadId() );
    Leave( why );
    return EXCEPTION_EXECUTE_HANDLER;
}

extern "C" HANDLE Guard_CreateThread( SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags )
{
    Start* s = (Start*)malloc( sizeof( Start ) );
    if( !s )
        return NULL;
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread( NULL, stack, Trampoline, s, flags, NULL );
    if( !h )
        free( s );
    return h;
}
