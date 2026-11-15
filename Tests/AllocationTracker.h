#pragma once

#include <atomic>

/** Counts heap allocations made on the current thread while a Scope is active.
    Used to prove the audio callback is allocation-free. */
namespace alloctrack
{
    extern thread_local bool tracking;
    extern std::atomic<long> allocations;

    struct Scope
    {
        Scope()  { tracking = true; }
        ~Scope() { tracking = false; }
    };
}
