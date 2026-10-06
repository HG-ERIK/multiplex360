#pragma once
#include <xtl.h>
#include <functional>
#include <deque>
#include <vector>

// work() runs on a worker thread, done() later inside Pump() on the render thread.
class TaskQueue
{
public:
    TaskQueue();
    ~TaskQueue();

    // cpus: hardware thread for each worker.
    void Start( const DWORD* cpus, int count );

    // 'tag' groups tasks so a screen can drop the ones it no longer needs (see Cancel).
    void Post( const std::function<void()>& work, const std::function<void()>& done, int tag = 0 );

    // Drops queued (not yet started) tasks with this tag; their done() never runs.
    void Cancel( int tag );

    // Render thread: runs finished tasks' done(). maxPerFrame limits the frame cost.
    void Pump( int maxPerFrame = 8 );

    size_t Pending() const;

private:
    struct Task
    {
        std::function<void()> work;
        std::function<void()> done;
        int                   tag;
    };

    static DWORD WINAPI Worker( LPVOID p );
    void Loop();

    mutable CRITICAL_SECTION m_lock;
    HANDLE                   m_wake;
    std::deque<Task>         m_queue;
    std::deque<Task>         m_finished;
    std::vector<HANDLE>      m_threads;
    volatile LONG            m_quit;
    volatile LONG            m_running;
};
