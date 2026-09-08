if(NOT DEFINED CASE OR NOT DEFINED EXPECTED OR NOT DEFINED TEST_BINARY_ROOT)
    message(FATAL_ERROR "Missing boundary fixture arguments")
endif()
# One unique tree per test invocation; no stale generated fixture can satisfy
# another run. Kept under ignored build outputs for failure diagnostics.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(binary "${TEST_BINARY_ROOT}/${CASE}-${nonce}")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_LIST_DIR}" -B "${binary}" -G "${GENERATOR}"
    "-DCASE=${CASE}" "-DGUARD_FILE=${GUARD_FILE}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=Debug"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors
    TIMEOUT 25)
set(log "${output}\n${errors}")
if(EXPECTED STREQUAL "PASS" OR EXPECTED STREQUAL "BUILD_INCLUDE")
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "Positive fixture failed (${status}):\n${log}")
    endif()
    if(EXPECTED STREQUAL "BUILD_INCLUDE")
        # Edit an existing file, not CMake input: the always-run build scan must
        # reject it even when no configure step is required.
        file(APPEND "${binary}/fixture-core/include/pcsx5/core/value.h" "#include <windows.h>\n")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${binary}" --parallel 2
        RESULT_VARIABLE build_status OUTPUT_VARIABLE output ERROR_VARIABLE errors
        TIMEOUT 15)
    set(log "${output}\n${errors}")
    if(EXPECTED STREQUAL "PASS" AND NOT build_status STREQUAL "0")
        message(FATAL_ERROR "Positive fixture did not compile (${build_status}):\n${log}")
    elseif(EXPECTED STREQUAL "BUILD_INCLUDE" AND
           (build_status STREQUAL "0" OR NOT log MATCHES "PCSX5_CORE_BOUNDARY\\[include\\]"))
        message(FATAL_ERROR "Post-config edit escaped build scan (${build_status}):\n${log}")
    endif()
elseif(status STREQUAL "0" OR NOT log MATCHES "PCSX5_CORE_BOUNDARY\\[${EXPECTED}\\]")
    message(FATAL_ERROR "Expected boundary diagnostic [${EXPECTED}], got ${status}:\n${log}")
endif()
message(STATUS "Boundary fixture ${CASE}: expected ${EXPECTED}")
