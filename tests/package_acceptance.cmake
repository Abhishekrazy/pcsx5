cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS BUILD_DIR CPACK_COMMAND EXECUTABLE_SUFFIX)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing required argument: ${required}")
    endif()
endforeach()

set(work_dir "${BUILD_DIR}/package-acceptance")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}/artifacts" "${work_dir}/extract")

execute_process(
    COMMAND "${CPACK_COMMAND}" --config "${BUILD_DIR}/CPackConfig.cmake"
        -G ZIP -B "${work_dir}/artifacts"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE package_result
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error)
if(NOT package_result EQUAL 0)
    message(FATAL_ERROR "CPack failed (${package_result}):\n${package_output}\n${package_error}")
endif()

file(GLOB archives LIST_DIRECTORIES false "${work_dir}/artifacts/*.zip")
file(GLOB checksums LIST_DIRECTORIES false "${work_dir}/artifacts/*.zip.sha256")
list(LENGTH archives archive_count)
list(LENGTH checksums checksum_count)
if(NOT archive_count EQUAL 1 OR NOT checksum_count EQUAL 1)
    message(FATAL_ERROR
        "Expected one ZIP and one SHA256 sidecar, found ${archive_count} and ${checksum_count}")
endif()
list(GET archives 0 archive)
list(GET checksums 0 checksum_file)

file(SHA256 "${archive}" actual_hash)
file(READ "${checksum_file}" checksum_text)
string(TOLOWER "${actual_hash}" actual_hash)
string(TOLOWER "${checksum_text}" checksum_text)
string(FIND "${checksum_text}" "${actual_hash}" hash_position)
if(NOT hash_position EQUAL 0)
    message(FATAL_ERROR "Package SHA256 sidecar does not match the ZIP")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xvf "${archive}"
    WORKING_DIRECTORY "${work_dir}/extract"
    RESULT_VARIABLE extract_result
    OUTPUT_QUIET ERROR_VARIABLE extract_error)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "Package extraction failed (${extract_result}): ${extract_error}")
endif()

file(GLOB package_roots LIST_DIRECTORIES true "${work_dir}/extract/*")
list(LENGTH package_roots root_count)
if(NOT root_count EQUAL 1 OR NOT IS_DIRECTORY "${package_roots}")
    message(FATAL_ERROR "Package must contain exactly one top-level directory")
endif()
list(GET package_roots 0 package_root)

file(GLOB_RECURSE packaged_files LIST_DIRECTORIES false
    RELATIVE "${package_root}" "${package_root}/*")
list(SORT packaged_files)
set(expected_files
    "bin/pcsx5${EXECUTABLE_SUFFIX}"
    "share/pcsx5/ARCHITECTURE.md"
    "share/pcsx5/LICENSE"
    "share/pcsx5/REBUILD_STATUS.md")
list(SORT expected_files)
if(NOT packaged_files STREQUAL expected_files)
    message(FATAL_ERROR
        "Unexpected package payload.\nExpected: ${expected_files}\nActual: ${packaged_files}")
endif()

set(packaged_executable "${package_root}/bin/pcsx5${EXECUTABLE_SUFFIX}")
execute_process(COMMAND "${packaged_executable}" --version
    RESULT_VARIABLE version_result OUTPUT_VARIABLE version_output ERROR_VARIABLE version_error)
if(NOT version_result EQUAL 0 OR
   NOT version_output MATCHES "experimental" OR
   NOT version_output MATCHES "no PS5 game compatibility claim")
    message(FATAL_ERROR
        "Packaged version/exclusion check failed (${version_result}): ${version_output}${version_error}")
endif()
execute_process(COMMAND "${packaged_executable}" --self-test
    RESULT_VARIABLE self_test_result OUTPUT_VARIABLE self_test_output ERROR_VARIABLE self_test_error)
if(NOT self_test_result EQUAL 0 OR NOT self_test_output MATCHES "PASS")
    message(FATAL_ERROR
        "Packaged self-test failed (${self_test_result}): ${self_test_output}${self_test_error}")
endif()

message(STATUS
    "Package acceptance PASS: exact experimental payload, SHA256, exclusions and self-test")
