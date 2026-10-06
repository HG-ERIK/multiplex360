/*
 * Minimal native pthreads for the Xbox 360 - just what libavcodec's threading uses.
 * Replaces pthreads-win32, which froze the console with FFmpeg frame threading.
 *
 *  - threads: CreateThread with a 1 MB stack (the 360 default is 64 KB, too small for
 *    the H.264 decoder)
 *  - mutexes: critical sections
 *  - condition variables: each waiter queues its own auto-reset event, so signal/broadcast
 *    wake exactly the threads that were waiting (no lost or stolen wakeups)
 */
#ifndef XB_PTHREAD_H
#define XB_PTHREAD_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xb_thread*  pthread_t;
typedef int                pthread_attr_t;
typedef int                pthread_mutexattr_t;
typedef int                pthread_condattr_t;
typedef CRITICAL_SECTION   pthread_mutex_t;

struct xb_cond_waiter;
typedef struct
{
    CRITICAL_SECTION        lock;
    struct xb_cond_waiter*  head;
    struct xb_cond_waiter*  tail;
} pthread_cond_t;

int    pthread_create( pthread_t* thread, const pthread_attr_t* attr, void* ( *start )( void* ), void* arg );
int    pthread_join( pthread_t thread, void** result );
HANDLE pthread_getw32threadhandle_np( pthread_t thread );

int    pthread_mutex_init( pthread_mutex_t* m, const pthread_mutexattr_t* attr );
int    pthread_mutex_destroy( pthread_mutex_t* m );
int    pthread_mutex_lock( pthread_mutex_t* m );
int    pthread_mutex_unlock( pthread_mutex_t* m );

int    pthread_cond_init( pthread_cond_t* c, const pthread_condattr_t* attr );
int    pthread_cond_destroy( pthread_cond_t* c );
int    pthread_cond_wait( pthread_cond_t* c, pthread_mutex_t* m );
int    pthread_cond_signal( pthread_cond_t* c );
int    pthread_cond_broadcast( pthread_cond_t* c );

#ifdef __cplusplus
}
#endif

#endif
