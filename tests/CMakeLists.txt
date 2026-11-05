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

# End to end: patch a running program and check its state survives.
find_package(Python3 COMPONENTS Interpreter)
if(TARGET hotpp AND Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The program is built from a copy in the build tree, which the test swaps for v2.
    set(case_dir ${CMAKE_CURRENT_BINARY_DIR}/e2e/live_patch)
    configure_file(e2e/live_patch/game_v1.cpp ${case_dir}/game.cpp COPYONLY)
    add_executable(hotpp_e2e_live_patch e2e/live_patch/main.cpp ${case_dir}/game.cpp)
    hotpp_enable(hotpp_e2e_live_patch)
    add_test(NAME e2e_live_patch
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_patch_case.py
            --host $<TARGET_FILE:hotpp>
            --exe $<TARGET_FILE:hotpp_e2e_live_patch>
            --source ${case_dir}/game.cpp
            --v1 ${CMAKE_CURRENT_SOURCE_DIR}/e2e/live_patch/game_v1.cpp
            --v2 ${CMAKE_CURRENT_SOURCE_DIR}/e2e/live_patch/game_v2.cpp)
    set_tests_properties(e2e_live_patch PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()
