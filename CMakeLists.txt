cmake_minimum_required(VERSION 3.25)
project(HotPP
    VERSION 0.1.0
    DESCRIPTION "Hot reload for C++ on Linux and Windows"
    LANGUAGES CXX)

option(HOTPP_BUILD_HOST     "Build the hotpp host executable" ON)
option(HOTPP_BUILD_EXAMPLES "Build the examples"              ${PROJECT_IS_TOP_LEVEL})
option(HOTPP_BUILD_TESTS    "Build the tests"                 ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "HotPP supports Linux and Windows only (got ${CMAKE_SYSTEM_NAME})")
endif()

find_package(Threads REQUIRED)
include(cmake/HotPP.cmake)

if(MSVC)
    set(HOTPP_WARNINGS /W4)
else()
    set(HOTPP_WARNINGS -Wall -Wextra)
endif()

# --- agent: static library linked into the target application ---------------
add_library(hotpp_agent STATIC
    src/agent/agent.cpp
    src/agent/region_select.cpp)
add_library(hotpp::agent ALIAS hotpp_agent)

if(WIN32)
    target_sources(hotpp_agent PRIVATE src/agent/platform_win.cpp)
    target_link_libraries(hotpp_agent PRIVATE psapi)
else()
    target_sources(hotpp_agent PRIVATE src/agent/platform_linux.cpp)
    target_link_libraries(hotpp_agent PRIVATE ${CMAKE_DL_LIBS})
endif()

target_include_directories(hotpp_agent
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(hotpp_agent PUBLIC cxx_std_17)
target_compile_options(hotpp_agent PRIVATE ${HOTPP_WARNINGS})
target_link_libraries(hotpp_agent PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(hotpp_agent PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

# --- host: the hotpp executable ----------------------------------------------
if(HOTPP_BUILD_HOST)
    add_executable(hotpp src/host/main.cpp)
    if(WIN32)
        target_sources(hotpp PRIVATE src/host/platform_win.cpp)
        target_compile_definitions(hotpp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    else()
        target_sources(hotpp PRIVATE src/host/platform_linux.cpp)
    endif()
    target_include_directories(hotpp PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(hotpp PRIVATE cxx_std_20)
    target_compile_definitions(hotpp PRIVATE HOTPP_VERSION="${PROJECT_VERSION}")
    target_compile_options(hotpp PRIVATE ${HOTPP_WARNINGS})
    target_link_libraries(hotpp PRIVATE Threads::Threads)
endif()

if(HOTPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(HOTPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
