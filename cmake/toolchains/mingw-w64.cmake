# Cross-compile for Windows from Linux with MinGW-w64, and run tests under Wine.
#
# MinGW is not a supported HotPP target (MSVC and clang-cl are); this toolchain
# exists so the Windows code paths can be built and smoke-tested on Linux.
#
#   cmake -B build/mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Static runtime so the binaries run under Wine without copying MinGW DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

find_program(HOTPP_WINE wine)
if(HOTPP_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${HOTPP_WINE})
endif()
