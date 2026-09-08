cmake_minimum_required(VERSION 3.25)
execute_process(COMMAND "${WORKER_TEST}" --terminate-join
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${result}" STREQUAL "73" OR
   NOT "${output}" MATCHES "WORKER_JOIN_FAILURE_INJECTED" OR
   NOT "${output}" MATCHES "WORKER_JOIN_TERMINATED")
    message(FATAL_ERROR "Wrong worker failure route: ${result}\n${output}\n${error}")
endif()
