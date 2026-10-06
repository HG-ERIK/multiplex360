#include "CpuMeter.h"

CpuMeter::CpuMeter() : m_running( 0 )
{
    for( int i = 0; i < THREADS; ++i )
    {
        m_threads[i] = NULL;
        m_busy[i] = 0;
    }
}

CpuMeter::~CpuMeter()
{
    Stop();
}

void CpuMeter::Start()
{
    if( m_running )
        return;
    InterlockedExchange( &m_running, 1 );
    for( int i = 0; i < THREADS; ++i )
    {
        m_slots[i].owner = this;
        m_slots[i].index = i;
        m_threads[i] = CreateThread( NULL, 16 * 1024, Spin, &m_slots[i], CREATE_SUSPENDED, NULL );
        XSetThreadProcessor( m_threads[i], i );
        SetThreadPriority( m_threads[i], THREAD_PRIORITY_IDLE );
        ResumeThread( m_threads[i] );
    }
}

void CpuMeter::Stop()
{
    if( !m_running )
        return;
    InterlockedExchange( &m_running, 0 );
    for( int i = 0; i < THREADS; ++i )
    {
        if( m_threads[i] )
        {
            WaitForSingleObject( m_threads[i], 2000 );
            CloseHandle( m_threads[i] );
            m_threads[i] = NULL;
        }
        m_busy[i] = 0;
    }
}

DWORD WINAPI CpuMeter::Spin( LPVOID p )
{
    Slot* slot = (Slot*)p;
    CpuMeter* self = slot->owner;
    LARGE_INTEGER freq, start, last, now;
    QueryPerformanceFrequency( &freq );
    // A gap longer than this between our own timestamps means someone else ran.
    const LONGLONG gap = freq.QuadPart / 20000;           // 50 us
    const LONGLONG window = freq.QuadPart / 2;            // report every 0.5 s

    QueryPerformanceCounter( &start );
    last = start;
    LONGLONG idle = 0;
    while( self->m_running )
    {
        QueryPerformanceCounter( &now );
        LONGLONG d = now.QuadPart - last.QuadPart;
        if( d < gap )
            idle += d;
        last = now;
        if( now.QuadPart - start.QuadPart >= window )
        {
            float busy = 1.0f - (float)idle / (float)( now.QuadPart - start.QuadPart );
            self->m_busy[slot->index] = busy < 0 ? 0 : ( busy > 1 ? 1 : busy );
            start = now;
            idle = 0;
        }
    }
    return 0;
}
