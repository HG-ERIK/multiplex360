/* Crash and freeze protection, so the console never stays stuck when nobody is there to
   restart it. A crash on any guarded thread is logged and the app leaves to the dashboard
   (xbdm would otherwise hold the crashed app frozen); a watchdog does the same if the main
   loop stops for 30 s. Usable from C (FFmpeg's threads) and C++. */
#ifndef GUARD_H
#define GUARD_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the watchdog. */
void   Guard_Start( void );

/* Called once per main-loop frame. */
void   Guard_Beat( void );

/* __except filter: __except( Guard_Filter( GetExceptionCode(), GetExceptionInformation(), "where" ) ).
   Logs the crash and leaves to the dashboard; doesn't return in practice. */
int    Guard_Filter( unsigned long code, EXCEPTION_POINTERS* info, const char* where );

/* CreateThread whose thread function runs under Guard_Filter. */
HANDLE Guard_CreateThread( SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags );

#ifdef __cplusplus
}
#endif

#endif
