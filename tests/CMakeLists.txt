add_executable(pcsx5_core_smoke core_smoke.cpp)
target_link_libraries(pcsx5_core_smoke PRIVATE pcsx5_core)
pcsx5_enable_warnings(pcsx5_core_smoke)

add_test(NAME pcsx5_core_smoke COMMAND pcsx5_core_smoke)
set_tests_properties(pcsx5_core_smoke PROPERTIES TIMEOUT 30)

add_test(NAME pcsx5_build_preset_contract COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/build_preset_contract.cmake")
set_tests_properties(pcsx5_build_preset_contract PROPERTIES TIMEOUT 30)

set(boundary_cases
    positive:PASS windows:include posix:include ui:include vulkan:include
    metal:include legacy:include nested:include macro:preprocessor
    splice:preprocessor alternate:preprocessor import:preprocessor
    include_next:preprocessor escape:source link:property
    transitive_link:property interface_link:property external_source:source
    include_root:include_root inherited_include:include_root generated:source
    genex_source:source genex_include:include_root compile_option:option
    source_option:source directory_definition:property global_flags:option
    pch:property late_link:property false_link:property quoted_comment:include
    pragma_link:preprocessor tight_import:preprocessor tight_include_next:preprocessor
    cmake_header:include control_whitespace:preprocessor post_config_edit:BUILD_INCLUDE
    local_flags:option)
foreach(entry IN LISTS boundary_cases)
    string(REPLACE ":" ";" pair "${entry}")
    list(GET pair 0 case)
    list(GET pair 1 expected)
    add_test(NAME "pcsx5_boundary_${case}" COMMAND "${CMAKE_COMMAND}"
        "-DCASE=${case}" "-DEXPECTED=${expected}"
        "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/boundary"
        "-DGUARD_FILE=${CMAKE_SOURCE_DIR}/cmake/CoreBoundary.cmake"
        "-DGENERATOR=${CMAKE_GENERATOR}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/boundary/run_case.cmake")
    set_tests_properties("pcsx5_boundary_${case}" PROPERTIES TIMEOUT 30 LABELS boundary)
endforeach()

add_executable(pcsx5_guest_address_range guest_address_range_tests.cpp)
target_link_libraries(pcsx5_guest_address_range PRIVATE pcsx5_core)
pcsx5_enable_warnings(pcsx5_guest_address_range)
add_test(NAME pcsx5_guest_address_range COMMAND pcsx5_guest_address_range)
set_tests_properties(pcsx5_guest_address_range PROPERTIES TIMEOUT 30 LABELS core)

add_test(NAME pcsx5_guest_range_repeatability COMMAND "${CMAKE_COMMAND}"
    "-DRANGE_TEST=$<TARGET_FILE:pcsx5_guest_address_range>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/guest_range_repeatability.cmake")
set_tests_properties(pcsx5_guest_range_repeatability PROPERTIES TIMEOUT 30 LABELS core)
