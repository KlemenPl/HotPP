# hotpp_enable(<target> [NO_AGENT])
#
# Makes <target> hot-reloadable: applies the compiler and linker flags HotPP relies
# on (docs/DESIGN.md §4) and exports compile commands. Executables also link the
# HotPP agent; shared libraries get only the flags, and are patched through the
# agent of the program that loads them. NO_AGENT gives an executable only the flags,
# for programs that use the single header (single_include/hotpp/hotpp.h) instead.
#
# Requirements this function cannot enforce for you:
#   * use a generator that writes compile_commands.json (Ninja or Makefiles; not
#     a Visual Studio generator),
#   * don't link with --gc-sections and don't strip the binary,
#   * don't enable LTO / whole-program optimization (/GL, /LTCG) anywhere in the target.

include_guard(GLOBAL)

function(hotpp_enable target)
    cmake_parse_arguments(ARG "NO_AGENT" "" "" ${ARGN})
    if(NOT TARGET ${target})
        message(FATAL_ERROR "hotpp_enable: '${target}' is not a target")
    endif()

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        if(NOT ARG_NO_AGENT)
            if(NOT TARGET hotpp::agent)
                message(FATAL_ERROR "hotpp_enable: no hotpp::agent target; use add_subdirectory(HotPP) or find_package(HotPP)")
            endif()
            target_link_libraries(${target} PRIVATE hotpp::agent)
        endif()
        target_compile_definitions(${target} PRIVATE
            "HOTPP_DEFAULT_COMPILE_COMMANDS=\"${CMAKE_BINARY_DIR}/compile_commands.json\"")
        # The agent launches this host if none is running, so it needn't be on PATH.
        if(TARGET hotpp::hotpp)
            target_compile_definitions(${target} PRIVATE "HOTPP_DEFAULT_HOST=\"$<TARGET_FILE:hotpp::hotpp>\"")
        endif()
    elseif(NOT type MATCHES "SHARED_LIBRARY|MODULE_LIBRARY|STATIC_LIBRARY|OBJECT_LIBRARY")
        message(FATAL_ERROR "hotpp_enable: '${target}' is a ${type}; expected an executable or a library")
    endif()
    set_target_properties(${target} PROPERTIES
        EXPORT_COMPILE_COMMANDS ON
        INTERPROCEDURAL_OPTIMIZATION OFF)

    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # MSVC and clang-cl (docs/DESIGN.md §4):
        #   /Z7          debug info in the objects; the linker writes a full PDB
        #   /Gy          each function in its own section (needed for /FUNCTIONPADMIN)
        #   /hotpatch    every function starts with an instruction of 2+ bytes
        #   /FUNCTIONPADMIN  padding before every function for the patch-site jump
        #   /OPT:NOREF /OPT:NOICF  keep "unused" and identical functions separate
        #   /INCREMENTAL:NO  no incremental-link thunks in front of functions
        target_compile_options(${target} PRIVATE /Z7 /Gy /hotpatch)
        if(type STREQUAL "EXECUTABLE" OR type MATCHES "SHARED_LIBRARY|MODULE_LIBRARY")
            target_link_options(${target} PRIVATE /DEBUG:FULL /FUNCTIONPADMIN /OPT:NOREF /OPT:NOICF /INCREMENTAL:NO)
        endif()
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            message(WARNING "hotpp_enable: Visual Studio generators write no compile_commands.json; use Ninja")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # 5 bytes of padding before every function entry and 2 at it, for the patch-site jump.
        target_compile_options(${target} PRIVATE -g -fpatchable-function-entry=7,5)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -fno-ipa-icf)
        endif()
    else()
        message(WARNING "hotpp_enable: unsupported compiler '${CMAKE_CXX_COMPILER_ID}'; hot reload may not work")
    endif()
endfunction()
