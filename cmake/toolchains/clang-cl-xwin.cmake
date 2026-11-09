# Cross-compiles HotPP for Windows x64 from Linux with clang-cl and lld-link, against
# the MSVC CRT and Windows SDK unpacked by xwin (https://github.com/Jake-Shadle/xwin):
#
#   xwin --accept-license splat --output ~/.xwin
#   cmake -B build/win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/clang-cl-xwin.cmake
#   cmake --build build/win && ctest --test-dir build/win    # tests run under Wine
#
# HOTPP_XWIN_DIR: where xwin put the SDK (default ~/.xwin). HOTPP_LLVM_BIN: where
# clang-cl, llvm-lib and lld-link are (default: searched, then /usr/lib/llvm-*/bin).
# Without lld-link, Rust's rust-lld is used if rustup installed one.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(HOTPP_XWIN_DIR "$ENV{HOME}/.xwin" CACHE PATH "xwin splat output (crt/ and sdk/)")
file(GLOB _llvm_dirs /usr/lib/llvm-*/bin)
list(SORT _llvm_dirs ORDER DESCENDING)
set(HOTPP_LLVM_BIN "" CACHE PATH "Directory with clang-cl, llvm-lib and lld-link")
set(_hints ${HOTPP_LLVM_BIN} ${_llvm_dirs})

find_program(CMAKE_C_COMPILER clang-cl HINTS ${_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER clang-cl HINTS ${_hints} REQUIRED)
find_program(CMAKE_AR llvm-lib HINTS ${_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER llvm-rc HINTS ${_hints})
find_program(CMAKE_MT llvm-mt HINTS ${_hints})
find_program(HOTPP_LLD_LINK lld-link HINTS ${_hints})
if(NOT HOTPP_LLD_LINK)
    file(GLOB _rust_lld $ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld)
    if(NOT _rust_lld)
        message(FATAL_ERROR "clang-cl-xwin: no lld-link (install lld) and no rust-lld")
    endif()
    list(GET _rust_lld 0 _rust_lld)
    # rust-lld needs its flavor first; a wrapper gives CMake a plain lld-link.
    set(HOTPP_LLD_LINK "${CMAKE_BINARY_DIR}/lld-link")
    file(WRITE "${HOTPP_LLD_LINK}" "#!/bin/sh\nexec \"${_rust_lld}\" -flavor link \"$@\"\n")
    file(CHMOD "${HOTPP_LLD_LINK}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
endif()
set(CMAKE_LINKER "${HOTPP_LLD_LINK}" CACHE FILEPATH "" FORCE)

set(_x "${HOTPP_XWIN_DIR}")
set(_inc "-imsvc ${_x}/crt/include -imsvc ${_x}/sdk/include/ucrt -imsvc ${_x}/sdk/include/um -imsvc ${_x}/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc} /EHsc")
set(_lib "/libpath:${_x}/crt/lib/x86_64 /libpath:${_x}/sdk/lib/um/x86_64 /libpath:${_x}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Tests run under Wine, in a prefix of their own.
find_program(HOTPP_WINE wine)
if(HOTPP_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR env "WINEPREFIX=${CMAKE_BINARY_DIR}/wineprefix" WINEDEBUG=-all ${HOTPP_WINE})
endif()

# The static release runtime: xwin installs no debug runtime (msvcrtd.lib) and no
# /RTC support library, and Wine's own msvcp140.dll doesn't match the current STL
# headers (std::condition_variable never wakes).
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_C_FLAGS_DEBUG "/Zi /Ob0 /Od" CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Ob0 /Od" CACHE STRING "")
