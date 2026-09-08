add_test(NAME trace_contract COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_LIST_DIR}/trace-contract-tests.cmake")
add_test(NAME trace_sample COMMAND "${CMAKE_COMMAND}"
    "-DTRACE_FILE=${CMAKE_CURRENT_LIST_DIR}/synthetic-trace.json"
    -P "${CMAKE_CURRENT_LIST_DIR}/validate-trace.cmake")
set_tests_properties(trace_contract trace_sample PROPERTIES
    TIMEOUT 30 LABELS "characterization;trace")
add_test(NAME fixture_provenance COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_LIST_DIR}/validate-fixture-provenance.cmake")
set_tests_properties(fixture_provenance PROPERTIES
    TIMEOUT 30 LABELS "characterization;provenance")
