// MinGW-w64 compatibility for building JUCE 8 (which targets MSVC) with Clang + mingw-w64.
// Compiled into hype_juce only for MinGW builds.

#include <windows.h>
#include <dxgi.h>
#include <cerrno>
#include <cstring>
#include <ctime>

// MSVC's __uuidof on a template instance returns the uuid of its first template argument.
// JUCE uses that with its ComSmartPtr<T> (e.g. __uuidof (device)); mingw-w64 needs the
// corresponding specialisations of __mingw_uuidof spelled out.
namespace juce { template <class T> class ComSmartPtr; }
template <> const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGIDevice>>()  { return __mingw_uuidof<IDXGIDevice>(); }
template <> const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGISurface>>() { return __mingw_uuidof<IDXGISurface>(); }

// Speech API class id (Windows SDK sapi.h / sapi.lib); mingw-w64 declares it but has no library defining it.
extern "C" const CLSID CLSID_SpVoice = { 0x96749377, 0x3391, 0x11d2, { 0x9e, 0xe3, 0x00, 0xc0, 0x4f, 0x79, 0x73, 0x96 } };

// _get_tzname is a UCRT function; the classic MSVCRT runtime only exposes _tzname.
extern "C"
{
    static errno_t __cdecl hypeGetTzname (size_t* length, char* buffer, size_t size, int index)
    {
        if (index < 0 || index > 1)
            return EINVAL;
        const char* name = _tzname[index] != nullptr ? _tzname[index] : "";
        const size_t needed = std::strlen (name) + 1;
        if (length != nullptr)
            *length = needed;
        if (buffer != nullptr && size > 0)
        {
            const size_t n = needed < size ? needed : size;
            std::memcpy (buffer, name, n);
            buffer[n - 1] = '\0';
        }
        return 0;
    }

    // JUCE calls it through a dllimport declaration, i.e. via this import-table pointer.
    errno_t (__cdecl* __imp__get_tzname) (size_t*, char*, size_t, int) = hypeGetTzname;
}
