add_executable(pcsx5_core_smoke core_smoke.cpp)
target_link_libraries(pcsx5_core_smoke PRIVATE pcsx5_core)
pcsx5_enable_warnings(pcsx5_core_smoke)

add_test(NAME pcsx5_core_smoke COMMAND pcsx5_core_smoke)
set_tests_properties(pcsx5_core_smoke PROPERTIES TIMEOUT 30)

add_test(NAME pcsx5_build_preset_contract COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/build_preset_contract.cmake")
set_tests_properties(pcsx5_build_preset_contract PROPERTIES TIMEOUT 30)
