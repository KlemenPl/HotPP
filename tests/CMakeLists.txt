add_executable(hotpp_unit_tests
    unit/main.cpp
    unit/protocol_test.cpp
    unit/region_select_test.cpp)
target_link_libraries(hotpp_unit_tests PRIVATE hotpp_agent)
target_include_directories(hotpp_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hotpp_unit_tests PRIVATE cxx_std_20)
target_compile_options(hotpp_unit_tests PRIVATE ${HOTPP_WARNINGS})
add_test(NAME unit COMMAND hotpp_unit_tests)

if(TARGET hotpp_host_lib)
    target_sources(hotpp_unit_tests PRIVATE
        unit/compile_db_test.cpp
        unit/deps_test.cpp
        unit/events_test.cpp
        unit/json_test.cpp
        unit/patch_site_test.cpp
        unit/session_state_test.cpp)
    target_link_libraries(hotpp_unit_tests PRIVATE hotpp_mcp_lib hotpp_host_lib)
endif()
if(TARGET hotpp_host_lib AND WIN32)
    # A program built like a hot-reloadable one, whose object, image and PDB the
    # COFF tests read.
    add_executable(hotpp_test_coff_sample unit/data/coff_sample.cpp)
    hotpp_enable(hotpp_test_coff_sample NO_AGENT)
    target_compile_options(hotpp_test_coff_sample PRIVATE /Od /Gw)
    target_sources(hotpp_unit_tests PRIVATE unit/coff_test.cpp)
    target_compile_definitions(hotpp_unit_tests PRIVATE
        "HOTPP_TEST_COFF_OBJECT=\"$<TARGET_OBJECTS:hotpp_test_coff_sample>\""
        "HOTPP_TEST_COFF_IMAGE=\"$<TARGET_FILE:hotpp_test_coff_sample>\"")
    add_dependencies(hotpp_unit_tests hotpp_test_coff_sample)
endif()
if(TARGET hotpp_host_lib AND NOT WIN32)
    target_sources(hotpp_unit_tests PRIVATE unit/mcp_test.cpp)

    # The ELF tests link a real object file compiled the way patch compiles are.
    add_library(hotpp_test_link_sample OBJECT unit/data/link_sample.cpp)
    target_compile_options(hotpp_test_link_sample PRIVATE
        -O0 -fPIE -fpatchable-function-entry=7,5 -ffunction-sections -fdata-sections)
    target_sources(hotpp_unit_tests PRIVATE unit/dwarf_types_test.cpp unit/elf_test.cpp unit/linker_test.cpp
                                            unit/robustness_test.cpp)
    target_compile_definitions(hotpp_unit_tests PRIVATE
        "HOTPP_TEST_LINK_OBJECT=\"$<TARGET_OBJECTS:hotpp_test_link_sample>\""
        "HOTPP_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    add_dependencies(hotpp_unit_tests hotpp_test_link_sample)
endif()

# End to end: the host starts the example, which must connect, report its modules
# and a patch region within rel32 reach, then disconnect cleanly.
if(TARGET hotpp AND TARGET hotpp_example_basic)
    add_test(NAME e2e_handshake
        COMMAND hotpp -- $<TARGET_FILE:hotpp_example_basic> --frames 3 --interval-ms 20)
    set_tests_properties(e2e_handshake PROPERTIES
        TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "OUT OF RANGE;handshake .* failed")
endif()

# End to end: each case under e2e/cases runs under the host while run_case.py
# swaps in newer versions of its sources; see e2e/run_case.py.
find_package(Python3 COMPONENTS Interpreter)
if(TARGET hotpp AND Python3_Interpreter_FOUND)
    # SINGLE_HEADER: build the program with single_include/hotpp/hotpp.h instead of
    # linking hotpp_agent (the case provides hotpp_impl.cpp).
    # MCP: drive the program through hotpp mcp with mcp/run_mcp_test.py instead.
    function(hotpp_e2e_case name)
        cmake_parse_arguments(ARG "SINGLE_HEADER;MCP" "" "" ${ARGN})
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/e2e/cases/${name})
        set(work ${CMAKE_CURRENT_BINARY_DIR}/e2e/${name})
        # The program is built from copies in the build tree, which the test edits.
        file(GLOB v1_files RELATIVE ${src}/v1 CONFIGURE_DEPENDS ${src}/v1/*)
        # lib_*.cpp files go into a shared library the program links.
        set(sources ${src}/main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/e2e/common/e2e_main.cpp)
        # plugin_*.cpp files go into a module the program dlopen()s (path in HOTPP_E2E_PLUGIN).
        set(lib_sources)
        set(plugin_sources)
        foreach(f IN LISTS v1_files)
            configure_file(${src}/v1/${f} ${work}/${f} COPYONLY)
            if(f MATCHES "^lib_.*\\.cpp$")
                list(APPEND lib_sources ${work}/${f})
            elseif(f MATCHES "^plugin_.*\\.cpp$")
                list(APPEND plugin_sources ${work}/${f})
            elseif(f MATCHES "\\.cpp$")
                list(APPEND sources ${work}/${f})
            endif()
        endforeach()
        if(ARG_SINGLE_HEADER)
            list(APPEND sources ${src}/hotpp_impl.cpp)
        endif()
        add_executable(hotpp_e2e_${name} ${sources})
        target_include_directories(hotpp_e2e_${name} PRIVATE ${work} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/common)
        target_link_libraries(hotpp_e2e_${name} PRIVATE Threads::Threads)
        target_compile_features(hotpp_e2e_${name} PRIVATE cxx_std_17)
        target_compile_definitions(hotpp_e2e_${name} PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
        if(ARG_SINGLE_HEADER)
            target_include_directories(hotpp_e2e_${name} PRIVATE ${PROJECT_SOURCE_DIR}/single_include)
            target_link_libraries(hotpp_e2e_${name} PRIVATE ${CMAKE_DL_LIBS} $<$<PLATFORM_ID:Windows>:psapi>)
            hotpp_enable(hotpp_e2e_${name} NO_AGENT)
        else()
            hotpp_enable(hotpp_e2e_${name})
        endif()
        if(plugin_sources)
            add_library(hotpp_e2e_${name}_plugin MODULE ${plugin_sources})
            target_include_directories(hotpp_e2e_${name}_plugin PRIVATE ${work})
            hotpp_enable(hotpp_e2e_${name}_plugin)
            add_dependencies(hotpp_e2e_${name} hotpp_e2e_${name}_plugin)
            target_compile_definitions(hotpp_e2e_${name} PRIVATE
                "HOTPP_E2E_PLUGIN=\"$<TARGET_FILE:hotpp_e2e_${name}_plugin>\"")
            target_link_libraries(hotpp_e2e_${name} PRIVATE ${CMAKE_DL_LIBS})
        endif()
        if(lib_sources)
            add_library(hotpp_e2e_${name}_lib SHARED ${lib_sources})
            target_include_directories(hotpp_e2e_${name}_lib PRIVATE ${work})
            hotpp_enable(hotpp_e2e_${name}_lib)
            target_link_libraries(hotpp_e2e_${name} PRIVATE hotpp_e2e_${name}_lib)
        endif()
        if(ARG_MCP)
            set(runner ${CMAKE_CURRENT_SOURCE_DIR}/mcp/run_mcp_test.py)
        else()
            set(runner ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_case.py)
        endif()
        # A path, not the imported Python target: when cross-compiling, targets run
        # under the emulator. The runner gets the emulator itself, for the host.
        set(emulator)
        if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
            list(JOIN CMAKE_CROSSCOMPILING_EMULATOR "|" emulator_arg)
            set(emulator --emulator "${emulator_arg}")
        endif()
        add_test(NAME e2e_${name}
            COMMAND ${Python3_EXECUTABLE} ${runner}
                --host $<TARGET_FILE:hotpp> --exe $<TARGET_FILE:hotpp_e2e_${name}>
                --case ${src} --work ${work}
                --build-dir ${CMAKE_BINARY_DIR} --target hotpp_e2e_${name} ${emulator})
        set_tests_properties(e2e_${name} PROPERTIES TIMEOUT 240)
    endfunction()

    foreach(case
            state new_global header_inline virtual_change lambda_template exceptions
            thread_spin repeat tls restart_required compile_error hooks async layout_warning running_loop)
        hotpp_e2e_case(${case})
    endforeach()
    # Not on Windows yet: DLL patching, hot restart, host restarts.
    if(NOT WIN32)
        foreach(case shared_lib restart host_restart dlopen_plugin)
            hotpp_e2e_case(${case})
        endforeach()
    endif()

    # A program that uses only the single header (no hotpp_agent, no built-in
    # compile_commands path: the agent finds it next to the executable).
    hotpp_e2e_case(single_header SINGLE_HEADER)

    if(NOT WIN32)
        # An AI agent's view: hotpp mcp launching, patching, restarting and stopping it.
        hotpp_e2e_case(mcp MCP)

        # Stepping through patched code needs gdb.
        find_program(HOTPP_GDB gdb)
        if(HOTPP_GDB)
            hotpp_e2e_case(gdb_step)
        endif()
    endif()
endif()

# The single header must match the sources it's generated from.
if(Python3_Interpreter_FOUND)
    # A path, not the imported target: when cross-compiling, targets run under the emulator.
    add_test(NAME single_header_up_to_date
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/amalgamate.py --check)
endif()

# Install into a temporary prefix and build a find_package(HotPP) project against it.
if(Python3_Interpreter_FOUND AND TARGET hotpp AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME install_and_consume
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/install/run_install_test.py
            --build-dir ${CMAKE_BINARY_DIR} --consumer ${CMAKE_CURRENT_SOURCE_DIR}/install/consumer
            --cxx ${CMAKE_CXX_COMPILER})
    set_tests_properties(install_and_consume PROPERTIES TIMEOUT 300)
endif()
