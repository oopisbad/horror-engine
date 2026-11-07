# Cross-compile the engine for 64-bit Windows with llvm-mingw (UCRT).
#
# Usage (Linux host):
#   cmake -S . -B build-win64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#     -DMINGW_ROOT=/path/to/llvm-mingw
#   cmake --build build-win64 -j
#
# UCRT is the Windows 10/11 runtime. The executable statically links libc++,
# libunwind and the compiler runtime, so the package needs no MinGW DLLs: only
# the OS-provided opengl32/user32/gdi32 (and friends) at runtime.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# try_compile subprojects re-run this file but do not inherit arbitrary -D
# cache entries, so accept the root from the environment as well.
if(NOT MINGW_ROOT AND DEFINED ENV{MINGW_ROOT})
    set(MINGW_ROOT "$ENV{MINGW_ROOT}")
endif()
# try_compile subprojects are configured without the parent's -D cache and
# environment, so fall back to the toolchain the repo downloads into.
if(NOT MINGW_ROOT)
    get_filename_component(_hor_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    if(EXISTS "${_hor_root}/external/downloads/llvm-mingw")
        set(MINGW_ROOT "${_hor_root}/external/downloads/llvm-mingw")
    endif()
endif()
if(NOT MINGW_ROOT)
    message(FATAL_ERROR "Set MINGW_ROOT=/path/to/llvm-mingw (env, -D, or unpack it in external/downloads/llvm-mingw)")
endif()

set(_hor_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER "${MINGW_ROOT}/bin/${_hor_triple}-clang")
set(CMAKE_CXX_COMPILER "${MINGW_ROOT}/bin/${_hor_triple}-clang++")
set(CMAKE_RC_COMPILER "${MINGW_ROOT}/bin/${_hor_triple}-windres")

set(CMAKE_FIND_ROOT_PATH "${MINGW_ROOT}/${_hor_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static runtimes: no libc++/libunwind/libgcc DLLs beside the exe.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
