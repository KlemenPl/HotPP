cmake_minimum_required(VERSION 3.25)
project(HotPP
    VERSION 1.1.0
    DESCRIPTION "Hot reload for C++ on Linux"
    LANGUAGES CXX)

option(HOTPP_BUILD_HOST     "Build the hotpp host executable" ON)
option(HOTPP_BUILD_EXAMPLES "Build the examples"              ${PROJECT_IS_TOP_LEVEL})
option(HOTPP_BUILD_TESTS    "Build the tests"                 ${PROJECT_IS_TOP_LEVEL})
set(HOTPP_SANITIZE OFF CACHE STRING "Sanitize the host and unit tests: OFF, ON (ASan + UBSan) or thread (TSan)")

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "HotPP supports Linux on x86-64 only (got ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR})")
endif()

find_package(Threads REQUIRED)
include(cmake/HotPP.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(HOTPP_WARNINGS -Wall -Wextra)

# --- agent: static library linked into the target application ---------------
add_library(hotpp_agent STATIC
    src/agent/agent.cpp
    src/agent/jit_debug.cpp
    src/agent/patcher.cpp
    src/agent/platform_linux.cpp
    src/agent/region_select.cpp
    src/agent/threads_linux.cpp)
add_library(hotpp::agent ALIAS hotpp_agent)
target_link_libraries(hotpp_agent PRIVATE ${CMAKE_DL_LIBS})

target_include_directories(hotpp_agent
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(hotpp_agent PROPERTIES EXPORT_NAME agent)
target_compile_features(hotpp_agent PUBLIC cxx_std_17)
target_compile_options(hotpp_agent PRIVATE ${HOTPP_WARNINGS})
target_link_libraries(hotpp_agent PRIVATE Threads::Threads)

# --- host: the hotpp executable ----------------------------------------------
if(HOTPP_BUILD_HOST)
    # Everything but main(), so unit tests can link the host code.
    add_library(hotpp_host_lib STATIC
        src/host/compile_db.cpp
        src/host/control.cpp
        src/host/control_protocol.cpp
        src/host/deps.cpp
        src/host/events.cpp
        src/host/registry.cpp
        src/host/report.cpp
        src/host/session.cpp
        src/host/session_state.cpp
        src/host/elf/elf_file.cpp
        src/host/image/dwarf.cpp
        src/host/image/dwarf_reader.cpp
        src/host/image/elf_image.cpp
        src/host/link/linker.cpp
        src/host/link/patch_site.cpp
        src/host/link/symbol_index.cpp
        src/host/object/elf_object.cpp
        src/host/platform_linux.cpp
        src/host/util/json.cpp
        src/host/watcher_linux.cpp)
    target_include_directories(hotpp_host_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(hotpp_host_lib PUBLIC cxx_std_20)
    target_compile_definitions(hotpp_host_lib PUBLIC HOTPP_VERSION="${PROJECT_VERSION}")
    target_compile_options(hotpp_host_lib PRIVATE ${HOTPP_WARNINGS})
    target_link_libraries(hotpp_host_lib PUBLIC Threads::Threads)

    # The host parses arbitrary object files and debug info and runs several threads;
    # sanitize it (and everything linking it). Programs under test stay unsanitized.
    if(HOTPP_SANITIZE STREQUAL "thread")
        target_compile_options(hotpp_host_lib PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(hotpp_host_lib PUBLIC -fsanitize=thread)
    elseif(HOTPP_SANITIZE)
        target_compile_options(hotpp_host_lib PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer
                                                     -fno-sanitize-recover=undefined)
        target_link_options(hotpp_host_lib PUBLIC -fsanitize=address,undefined)
    endif()

    # hotpp mcp: the MCP server for AI agents (in the same executable).
    add_library(hotpp_mcp_lib STATIC
        src/mcp/hosts.cpp
        src/mcp/main.cpp
        src/mcp/server.cpp
        src/mcp/tools.cpp)
    target_compile_options(hotpp_mcp_lib PRIVATE ${HOTPP_WARNINGS})
    target_link_libraries(hotpp_mcp_lib PUBLIC hotpp_host_lib)

    add_executable(hotpp src/host/main.cpp)
    add_executable(hotpp::hotpp ALIAS hotpp)
    target_compile_options(hotpp PRIVATE ${HOTPP_WARNINGS})
    target_link_libraries(hotpp PRIVATE hotpp_mcp_lib hotpp_host_lib)
endif()

# Regenerates single_include/hotpp/hotpp.h from the agent sources.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(hotpp_single_header
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/amalgamate.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generating single_include/hotpp/hotpp.h")
endif()

# --- install: host, agent, headers and a CMake package -----------------------
#   find_package(HotPP) gives hotpp::agent, hotpp::hotpp and hotpp_enable().
install(TARGETS hotpp_agent EXPORT HotPPTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(HOTPP_BUILD_HOST)
    install(TARGETS hotpp EXPORT HotPPTargets RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES include/hotpp/hotpp.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hotpp)
install(FILES LICENSE README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES single_include/hotpp/hotpp.h DESTINATION ${CMAKE_INSTALL_DATADIR}/hotpp/single_include/hotpp)
set(HOTPP_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/HotPP)
install(EXPORT HotPPTargets NAMESPACE hotpp:: DESTINATION ${HOTPP_CMAKE_DIR})
configure_package_config_file(cmake/HotPPConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/HotPPConfig.cmake
    INSTALL_DESTINATION ${HOTPP_CMAKE_DIR})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/HotPPConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/HotPPConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/HotPPConfigVersion.cmake
              cmake/HotPP.cmake
        DESTINATION ${HOTPP_CMAKE_DIR})

if(HOTPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(HOTPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
