#include "Tasks.h"
#include "Guard.h"

TaskQueue::TaskQueue() : m_quit( 0 ), m_running( 0 )
{
    InitializeCriticalSection( &m_lock );
    m_wake = CreateEvent( NULL, FALSE, FALSE, NULL );
}

TaskQueue::~TaskQueue()
{
    InterlockedExchange( &m_quit, 1 );
    for( size_t i = 0; i < m_threads.size(); ++i )
        SetEvent( m_wake );
    for( size_t i = 0; i < m_threads.size(); ++i )
    {
        WaitForSingleObject( m_threads[i], 5000 );
        CloseHandle( m_threads[i] );
    }
    CloseHandle( m_wake );
    DeleteCriticalSection( &m_lock );
}

void TaskQueue::Start( const DWORD* cpus, int count )
{
    for( int i = 0; i < count; ++i )
    {
        HANDLE h = Guard_CreateThread( 256 * 1024, Worker, this, CREATE_SUSPENDED );
        XSetThreadProcessor( h, cpus[i] );
        ResumeThread( h );
        m_threads.push_back( h );
    }
}

void TaskQueue::Post( const std::function<void()>& work, const std::function<void()>& done, int tag )
{
    Task t;
    t.work = work;
    t.done = done;
    t.tag = tag;
    EnterCriticalSection( &m_lock );
    m_queue.push_back( t );
    LeaveCriticalSection( &m_lock );
    SetEvent( m_wake );
}

void TaskQueue::Cancel( int tag )
{
    EnterCriticalSection( &m_lock );
    for( std::deque<Task>::iterator it = m_queue.begin(); it != m_queue.end(); )
    {
        if( it->tag == tag )
            it = m_queue.erase( it );
        else
            ++it;
    }
    LeaveCriticalSection( &m_lock );
}

size_t TaskQueue::Pending() const
{
    EnterCriticalSection( &m_lock );
    size_t n = m_queue.size() + m_finished.size() + (size_t)m_running;
    LeaveCriticalSection( &m_lock );
    return n;
}

DWORD WINAPI TaskQueue::Worker( LPVOID p )
{
    ( (TaskQueue*)p )->Loop();
    return 0;
}

void TaskQueue::Loop()
{
    while( !m_quit )
    {
        Task t;
        bool have = false;
        EnterCriticalSection( &m_lock );
        if( !m_queue.empty() )
        {
            t = m_queue.front();
            m_queue.pop_front();
            have = true;
            ++m_running;
        }
        LeaveCriticalSection( &m_lock );

        if( !have )
        {
            WaitForSingleObject( m_wake, 100 );
            continue;
        }

        if( t.work )
            t.work();

        EnterCriticalSection( &m_lock );
        --m_running;
        if( t.done )
            m_finished.push_back( t );
        LeaveCriticalSection( &m_lock );
    }
}

void TaskQueue::Pump( int maxPerFrame )
{
    for( int i = 0; i < maxPerFrame; ++i )
    {
        Task t;
        EnterCriticalSection( &m_lock );
        bool have = !m_finished.empty();
        if( have )
        {
            t = m_finished.front();
            m_finished.pop_front();
        }
        LeaveCriticalSection( &m_lock );
        if( !have )
            return;
        t.done();
    }
}
