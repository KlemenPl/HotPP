# hotpp_enable(<target>)
#
# Makes <target> hot-reloadable: applies the compiler and linker flags HotPP relies
# on (docs/DESIGN.md §4) and exports compile commands. Executables also link the
# HotPP agent; shared libraries get only the flags, and are patched through the
# agent of the program that loads them.
#
# Requirements this function cannot enforce for you:
#   * use a generator that writes compile_commands.json (Ninja or Makefiles),
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

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_link_libraries(${target} PRIVATE hotpp_agent)
        target_compile_definitions(${target} PRIVATE
            "HOTPP_DEFAULT_COMPILE_COMMANDS=\"${CMAKE_BINARY_DIR}/compile_commands.json\"")
    elseif(NOT type MATCHES "SHARED_LIBRARY|MODULE_LIBRARY|STATIC_LIBRARY|OBJECT_LIBRARY")
        message(FATAL_ERROR "hotpp_enable: '${target}' is a ${type}; expected an executable or a library")
    endif()
    set_target_properties(${target} PROPERTIES
        EXPORT_COMPILE_COMMANDS ON
        INTERPROCEDURAL_OPTIMIZATION OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # 5 bytes of padding before every function entry and 2 at it, for the patch-site jump.
        target_compile_options(${target} PRIVATE -g -fpatchable-function-entry=7,5)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -fno-ipa-icf)
        endif()
    else()
        message(WARNING "hotpp_enable: unsupported compiler '${CMAKE_CXX_COMPILER_ID}'; hot reload may not work")
    endif()
endfunction()
