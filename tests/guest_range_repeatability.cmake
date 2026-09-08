if(NOT DEFINED RANGE_TEST OR NOT EXISTS "${RANGE_TEST}")
    message(FATAL_ERROR "Missing guest range test executable")
endif()

foreach(run RANGE 1 2)
    execute_process(COMMAND "${RANGE_TEST}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors
        TIMEOUT 10)
    if(NOT status STREQUAL "0" OR NOT errors STREQUAL "")
        message(FATAL_ERROR "Range run ${run} failed (${status}): ${errors}\n${output}")
    endif()
    # CTest timings and JUnit timestamps are diagnostics, not deterministic data.
    # Only the executable's asserted semantic summary is compared here.
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT output MATCHES "^guest_address_range: [1-9][0-9]* checks, 0 failures\n$")
        message(FATAL_ERROR "Missing range-test success summary: ${output}")
    endif()
    if(run EQUAL 2 AND NOT output STREQUAL previous)
        message(FATAL_ERROR "Range-test summaries differ across runs")
    endif()
    set(previous "${output}")
endforeach()
message(STATUS "Two independent range-test runs produced identical checked summaries")
