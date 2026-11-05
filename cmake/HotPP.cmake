# hotpp_enable(<target>)
#
# Makes <target> hot-reloadable: links the HotPP agent, applies the compiler and
# linker flags HotPP relies on (docs/DESIGN.md §4) and exports compile commands.
#
# Requirements this function cannot enforce for you:
#   * use a generator that writes compile_commands.json (Ninja or Makefiles;
#     Visual Studio generators do not),
#   * don't link with --gc-sections and don't strip the binary,
#   * don't enable LTO / whole-program optimization anywhere in the target.

include_guard(GLOBAL)

function(hotpp_enable target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "hotpp_enable: '${target}' is not a target")
    endif()
    if(NOT TARGET hotpp_agent)
        message(FATAL_ERROR "hotpp_enable: the hotpp_agent target is missing; add HotPP with add_subdirectory() first")
    endif()

    target_link_libraries(${target} PRIVATE hotpp_agent)
    set_target_properties(${target} PROPERTIES
        EXPORT_COMPILE_COMMANDS ON
        INTERPROCEDURAL_OPTIMIZATION OFF)
    target_compile_definitions(${target} PRIVATE
        "HOTPP_DEFAULT_COMPILE_COMMANDS=\"${CMAKE_BINARY_DIR}/compile_commands.json\"")

    if(MSVC) # MSVC and clang-cl
        # /Z7: debug info inside each .obj, so patch compiles never contend for a shared PDB.
        set_property(TARGET ${target} PROPERTY MSVC_DEBUG_INFORMATION_FORMAT Embedded)
        target_compile_options(${target} PRIVATE /Z7 /Gy /hotpatch)
        target_link_options(${target} PRIVATE
            /DEBUG:FULL /FUNCTIONPADMIN /OPT:NOREF /OPT:NOICF /INCREMENTAL:NO)
    elseif(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # MinGW: only used to smoke-test HotPP's Windows code from Linux. Binaries built
        # with -fpatchable-function-entry fail at startup there, so no patch padding.
        message(WARNING "hotpp_enable(${target}): MinGW is not a supported HotPP toolchain; "
                        "the target connects to the host but cannot be patched")
        target_compile_options(${target} PRIVATE -g)
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
