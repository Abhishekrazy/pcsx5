# Runs a guest smoke test and requires BOTH a zero exit status AND the expected
# marker in the output.
#
# CTest's PASS_REGULAR_EXPRESSION *replaces* the return-code check rather than
# adding to it. Both guest smoke tests used it alone, so each reported Passed
# while the emulator printed "FATAL: abort() raised (signal 22) - process is
# terminating" and exited non-zero. A test that cannot fail when the process it
# runs dies is not a test, and two of the suite's fifty were in that state.
#
# Invoked as:
#   cmake -DEXE=<pcsx5_cli> -DELF=<guest elf> -DREPORT=<json path>
#         -DMARKER=<expected text> -P run_guest_smoke.cmake

foreach(required EXE ELF REPORT MARKER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_guest_smoke.cmake: -D${required}= is required")
    endif()
endforeach()

execute_process(
    COMMAND "${EXE}" --strict-imports "--report=${REPORT}" "${ELF}"
    OUTPUT_VARIABLE smoke_stdout
    ERROR_VARIABLE  smoke_stderr
    RESULT_VARIABLE smoke_result
)

# Always surface the child's output: on failure it is the evidence, and on
# success it is what a reader checks the marker against.
message(STATUS "${smoke_stdout}")
if(NOT smoke_stderr STREQUAL "")
    message(STATUS "${smoke_stderr}")
endif()

string(FIND "${smoke_stdout}${smoke_stderr}" "${MARKER}" marker_at)

# A fatal abort must fail the test even when the process still exits 0, which
# is what this emulator does today: it logs a FATAL abort and then returns
# success. Checking only the exit status would let that through, leaving these
# tests unable to fail on the very crash they exist to catch.
string(FIND "${smoke_stdout}${smoke_stderr}" "FATAL:" fatal_at)

# Report both conditions rather than the first to fail, so a run that loses the
# marker AND crashes does not look like a simple crash.
set(failures "")
if(NOT smoke_result EQUAL 0)
    set(failures "${failures}\n  - process exited with '${smoke_result}' (expected 0)")
endif()
if(marker_at EQUAL -1)
    set(failures "${failures}\n  - marker '${MARKER}' not present in output")
endif()
if(NOT fatal_at EQUAL -1)
    set(failures "${failures}\n  - output contains a FATAL: line; the guest run aborted")
endif()

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "guest smoke test failed:${failures}")
endif()
