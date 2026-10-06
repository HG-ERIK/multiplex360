#pragma once
#include <xtl.h>

// Per-hardware-thread load for the stats overlay: an idle-priority thread on each one
// timestamps itself; gaps are time other threads used.
class CpuMeter
{
public:
    enum { THREADS = 6 };

    CpuMeter();
    ~CpuMeter();

    void  Start();
    void  Stop();
    bool  Running() const { return m_running != 0; }

    // Busy fraction 0..1 of hardware thread i over the last half second.
    float Busy( int i ) const { return m_busy[i]; }

private:
    static DWORD WINAPI Spin( LPVOID p );

    struct Slot
    {
        CpuMeter* owner;
        int       index;
    };

    Slot          m_slots[THREADS];
    HANDLE        m_threads[THREADS];
    volatile LONG m_running;
    volatile float m_busy[THREADS];
};
