cmake_minimum_required(VERSION 3.25)
execute_process(COMMAND "${FIXTURE}" sys_exit_child
    WORKING_DIRECTORY "${OUTPUT_DIR}" TIMEOUT 5
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result STREQUAL "42")
    message(FATAL_ERROR "Real SysExit must terminate with 42, got ${result}\n${output}\n${errors}")
endif()
string(REGEX MATCHALL "exit_hook=[0-9]+" markers "${output}")
list(LENGTH markers count)
if(NOT count EQUAL 1 OR NOT markers STREQUAL "exit_hook=1")
    message(FATAL_ERROR "Expected exactly one flushed real exit-hook observation\n${output}")
endif()
message(STATUS "Real SysExit terminated child with 42 after exactly one exit-hook observation")
