#include "AllocationTracker.h"

#include <cstddef>

namespace alloctrack
{
    thread_local bool tracking = false;
    std::atomic<long> allocations { 0 };
}

#if defined (__GLIBC__) && ! defined (__SANITIZE_ADDRESS__)
// Interpose the C allocator itself, so JUCE's HeapBlock (malloc-based) and operator new
// (which calls malloc in libstdc++) are both caught.
extern "C"
{
    void* __libc_malloc (size_t);
    void* __libc_calloc (size_t, size_t);
    void* __libc_realloc (void*, size_t);
    void* __libc_memalign (size_t, size_t);

    static inline void noteAllocation()
    {
        if (alloctrack::tracking)
            alloctrack::allocations.fetch_add (1, std::memory_order_relaxed);
    }

    void* malloc (size_t s)                 { noteAllocation(); return __libc_malloc (s); }
    void* calloc (size_t n, size_t s)       { noteAllocation(); return __libc_calloc (n, s); }
    void* realloc (void* p, size_t s)       { noteAllocation(); return __libc_realloc (p, s); }
    void* memalign (size_t a, size_t s)     { noteAllocation(); return __libc_memalign (a, s); }
    void* aligned_alloc (size_t a, size_t s){ noteAllocation(); return __libc_memalign (a, s); }

    int posix_memalign (void** out, size_t a, size_t s)
    {
        noteAllocation();
        *out = __libc_memalign (a, s);
        return *out != nullptr ? 0 : 12 /* ENOMEM */;
    }
}
#else
 // Not glibc, or running under AddressSanitizer (which owns malloc): allocation
 // tracking is disabled and the real-time allocation test passes trivially.
#endif
