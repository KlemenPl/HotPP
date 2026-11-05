add_executable(hotpp_unit_tests
    unit/main.cpp
    unit/protocol_test.cpp
    unit/region_select_test.cpp)
target_link_libraries(hotpp_unit_tests PRIVATE hotpp_agent)
target_include_directories(hotpp_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hotpp_unit_tests PRIVATE cxx_std_17)
target_compile_options(hotpp_unit_tests PRIVATE ${HOTPP_WARNINGS})
add_test(NAME unit COMMAND hotpp_unit_tests)

# End to end: the host starts the example, which must connect, report its modules
# and a patch region within rel32 reach, then disconnect cleanly.
if(TARGET hotpp AND TARGET hotpp_example_basic)
    add_test(NAME e2e_handshake
        COMMAND hotpp -- $<TARGET_FILE:hotpp_example_basic> --frames 3 --interval-ms 20)
    set_tests_properties(e2e_handshake PROPERTIES
        TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "OUT OF RANGE;handshake .* failed")
endif()
