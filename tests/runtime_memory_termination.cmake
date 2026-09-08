cmake_minimum_required(VERSION 3.25)
execute_process(COMMAND "${MEMORY_TEST}" destructor-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${result}" STREQUAL "73" OR
   NOT "${output}" MATCHES "INJECTED_RELEASE_FAILURE" OR
   NOT "${output}" MATCHES "EXPECTED_DESTRUCTOR_TERMINATION")
    message(FATAL_ERROR "Wrong destructor failure route: ${result}\n${output}\n${error}")
endif()
