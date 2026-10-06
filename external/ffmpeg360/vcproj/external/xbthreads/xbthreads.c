/* See pthread.h. */
#include <xtl.h>
#include <stdlib.h>
#include "pthread.h"

/* Provided by the app (src/Guard.cpp): logs a crash and leaves to the dashboard. */
int Guard_Filter( unsigned long code, EXCEPTION_POINTERS* info, const char* where );

#define XB_THREAD_STACK ( 1024 * 1024 )

struct xb_thread
{
    HANDLE  handle;
    void* ( *start )( void* );
    void*   arg;
    void*   result;
};

struct xb_cond_waiter
{
    HANDLE                  event;
    struct xb_cond_waiter*  next;
};

static DWORD g_eventSlot = TLS_OUT_OF_INDEXES;
static LONG  g_slotInit = 0;

/* Each thread's private auto-reset event used for condition waits. */
static HANDLE ThreadEvent( void )
{
    HANDLE ev;
    if( g_slotInit != 2 )
    {
        if( InterlockedCompareExchange( &g_slotInit, 1, 0 ) == 0 )
        {
            g_eventSlot = TlsAlloc();
            InterlockedExchange( &g_slotInit, 2 );
        }
        while( g_slotInit != 2 )
            Sleep( 0 );
    }
    ev = (HANDLE)TlsGetValue( g_eventSlot );
    if( !ev )
    {
        ev = CreateEvent( NULL, FALSE, FALSE, NULL );
        TlsSetValue( g_eventSlot, ev );
    }
    return ev;
}

static DWORD WINAPI ThreadMain( LPVOID p )
{
    struct xb_thread* t = (struct xb_thread*)p;
    HANDLE ev;
    __try
    {
        t->result = t->start( t->arg );
    }
    __except( Guard_Filter( GetExceptionCode(), GetExceptionInformation(), "ffmpeg" ) )
    {
    }
    ev = g_slotInit == 2 ? (HANDLE)TlsGetValue( g_eventSlot ) : NULL;
    if( ev )
        CloseHandle( ev );
    return 0;
}

int pthread_create( pthread_t* thread, const pthread_attr_t* attr, void* ( *start )( void* ), void* arg )
{
    struct xb_thread* t = (struct xb_thread*)calloc( 1, sizeof( *t ) );
    (void)attr;
    if( !t )
        return -1;
    t->start = start;
    t->arg = arg;
    t->handle = CreateThread( NULL, XB_THREAD_STACK, ThreadMain, t, 0, NULL );
    if( !t->handle )
    {
        free( t );
        return -1;
    }
    *thread = t;
    return 0;
}

int pthread_join( pthread_t t, void** result )
{
    if( !t )
        return -1;
    WaitForSingleObject( t->handle, INFINITE );
    CloseHandle( t->handle );
    if( result )
        *result = t->result;
    free( t );
    return 0;
}

HANDLE pthread_getw32threadhandle_np( pthread_t t )
{
    return t ? t->handle : NULL;
}

int pthread_mutex_init( pthread_mutex_t* m, const pthread_mutexattr_t* attr )
{
    (void)attr;
    InitializeCriticalSection( m );
    return 0;
}

int pthread_mutex_destroy( pthread_mutex_t* m )
{
    DeleteCriticalSection( m );
    return 0;
}

int pthread_mutex_lock( pthread_mutex_t* m )
{
    EnterCriticalSection( m );
    return 0;
}

int pthread_mutex_unlock( pthread_mutex_t* m )
{
    LeaveCriticalSection( m );
    return 0;
}

int pthread_cond_init( pthread_cond_t* c, const pthread_condattr_t* attr )
{
    (void)attr;
    InitializeCriticalSection( &c->lock );
    c->head = c->tail = NULL;
    return 0;
}

int pthread_cond_destroy( pthread_cond_t* c )
{
    DeleteCriticalSection( &c->lock );
    return 0;
}

int pthread_cond_wait( pthread_cond_t* c, pthread_mutex_t* m )
{
    /* The node lives on this stack; signal/broadcast unlink it before setting the event,
       so it is never touched after we wake up. */
    struct xb_cond_waiter self;
    self.event = ThreadEvent();
    self.next = NULL;

    EnterCriticalSection( &c->lock );
    if( c->tail )
        c->tail->next = &self;
    else
        c->head = &self;
    c->tail = &self;
    LeaveCriticalSection( &c->lock );

    LeaveCriticalSection( m );
    WaitForSingleObject( self.event, INFINITE );
    EnterCriticalSection( m );
    return 0;
}

int pthread_cond_signal( pthread_cond_t* c )
{
    struct xb_cond_waiter* w;
    EnterCriticalSection( &c->lock );
    w = c->head;
    if( w )
    {
        c->head = w->next;
        if( !c->head )
            c->tail = NULL;
    }
    LeaveCriticalSection( &c->lock );
    if( w )
        SetEvent( w->event );
    return 0;
}

int pthread_cond_broadcast( pthread_cond_t* c )
{
    struct xb_cond_waiter* w;
    EnterCriticalSection( &c->lock );
    w = c->head;
    c->head = c->tail = NULL;
    LeaveCriticalSection( &c->lock );
    while( w )
    {
        /* Read next before waking: the woken thread's node goes away with its stack frame. */
        struct xb_cond_waiter* next = w->next;
        SetEvent( w->event );
        w = next;
    }
    return 0;
}

/* FFPlay360's libavcodec calls pthreads-win32's static-library setup hooks directly
   (implicitly declared, so int-returning). Nothing to set up here. */
int ptw32_processInitialize( void ) { return 1; }
int ptw32_processTerminate( void )  { return 0; }
