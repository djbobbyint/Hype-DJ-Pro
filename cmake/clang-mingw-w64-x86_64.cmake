# Cross-compile HYPE DJ PRO for 64-bit Windows from Linux with the system Clang targeting
# MinGW-w64 (uses the distribution's mingw-w64 headers, CRT and libstdc++).
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-mingw-w64-x86_64.cmake ...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(triple x86_64-w64-windows-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_SYSROOT /usr/x86_64-w64-mingw32)
# Use the posix-threads flavour of the mingw-w64 GCC runtime (std::thread, std::mutex).
set(gcc_dir /usr/lib/gcc/x86_64-w64-mingw32/13-posix)
# -femulated-tls: match the thread_local ABI of the GCC-built mingw-w64 libstdc++.
set(CMAKE_C_FLAGS_INIT   "-B${gcc_dir} -femulated-tls")
set(CMAKE_CXX_FLAGS_INIT "-B${gcc_dir} -femulated-tls -isystem /usr/lib/gcc/x86_64-w64-mingw32/13-posix/include/c++ -isystem /usr/lib/gcc/x86_64-w64-mingw32/13-posix/include/c++/x86_64-w64-mingw32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -L${gcc_dir}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -L${gcc_dir}")
set(CMAKE_C_STANDARD_LIBRARIES "-lpthread")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lpthread")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
