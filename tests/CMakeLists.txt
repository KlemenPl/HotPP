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
        unit/json_test.cpp
        unit/patch_site_test.cpp)
    target_link_libraries(hotpp_unit_tests PRIVATE hotpp_host_lib)

    # The ELF tests link a real object file compiled the way patch compiles are.
    if(NOT WIN32)
        add_library(hotpp_test_link_sample OBJECT unit/data/link_sample.cpp)
        target_compile_options(hotpp_test_link_sample PRIVATE
            -O0 -fPIE -fpatchable-function-entry=7,5 -ffunction-sections -fdata-sections)
        target_sources(hotpp_unit_tests PRIVATE unit/elf_test.cpp unit/linker_test.cpp)
        target_compile_definitions(hotpp_unit_tests PRIVATE
            "HOTPP_TEST_LINK_OBJECT=\"$<TARGET_OBJECTS:hotpp_test_link_sample>\""
            "HOTPP_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
        add_dependencies(hotpp_unit_tests hotpp_test_link_sample)
    endif()
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
if(TARGET hotpp AND Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    function(hotpp_e2e_case name)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/e2e/cases/${name})
        set(work ${CMAKE_CURRENT_BINARY_DIR}/e2e/${name})
        # The program is built from copies in the build tree, which the test edits.
        file(GLOB v1_files RELATIVE ${src}/v1 CONFIGURE_DEPENDS ${src}/v1/*)
        set(sources ${src}/main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/e2e/common/e2e_main.cpp)
        foreach(f IN LISTS v1_files)
            configure_file(${src}/v1/${f} ${work}/${f} COPYONLY)
            if(f MATCHES "\\.cpp$")
                list(APPEND sources ${work}/${f})
            endif()
        endforeach()
        add_executable(hotpp_e2e_${name} ${sources})
        target_include_directories(hotpp_e2e_${name} PRIVATE ${work} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/common)
        target_link_libraries(hotpp_e2e_${name} PRIVATE Threads::Threads)
        hotpp_enable(hotpp_e2e_${name})
        add_test(NAME e2e_${name}
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_case.py
                --host $<TARGET_FILE:hotpp> --exe $<TARGET_FILE:hotpp_e2e_${name}>
                --case ${src} --work ${work})
        set_tests_properties(e2e_${name} PROPERTIES TIMEOUT 180)
    endfunction()

    foreach(case
            state new_global header_inline virtual_change lambda_template exceptions
            thread_spin repeat tls restart_required compile_error)
        hotpp_e2e_case(${case})
    endforeach()
endif()
