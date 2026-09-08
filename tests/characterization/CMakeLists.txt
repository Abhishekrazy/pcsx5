cmake_minimum_required(VERSION 3.25)
project(pcsx5_legacy_characterization LANGUAGES CXX)

# Independent Phase 0 build: never linked into the new emulator core.
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Legacy characterization requires Windows x64 MSVC")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
get_filename_component(legacy_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(legacy_memory STATIC
    "${legacy_root}/src/memory/memory.cpp"
    "${legacy_root}/src/common/log.cpp")
target_include_directories(legacy_memory PUBLIC "${legacy_root}/src")
target_compile_options(legacy_memory PRIVATE /W4)

function(add_characterization name source)
    add_executable(${name} "${legacy_root}/tests/${source}" ${ARGN})
    target_link_libraries(${name} PRIVATE legacy_memory)
    target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4>")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runs/${name}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS "legacy;characterization"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runs/${name}")
endfunction()

add_characterization(memory_validation memory_validation.cpp)
add_characterization(memory_query memory_query_tests.cpp)
add_characterization(guest_memory_access guest_memory_access_tests.cpp)
add_characterization(memory_write_tracker memory_write_tracker_tests.cpp)
add_characterization(kernel_memory kernel_memory_tests.cpp "${legacy_root}/src/kernel/memory.cpp")
add_characterization(tls_context tls_context_tests.cpp "${legacy_root}/src/kernel/tls.cpp")
add_characterization(tls_patch_stub tls_patch_stub_tests.cpp "${legacy_root}/src/kernel/tls_patch.cpp")
target_compile_definitions(tls_patch_stub PRIVATE PCSX5_TLS_PATCH_FULL)

enable_language(ASM_MASM)
add_characterization(dispatcher_execution characterization/dispatcher_execution.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/dispatcher_fixture.asm"
    "${legacy_root}/src/hle/dispatcher.asm")
add_characterization(memory_fault_routes characterization/memory_fault_routes.cpp)
add_characterization(tls_thread_execution characterization/tls_thread_execution.cpp
    "${legacy_root}/src/kernel/tls_patch.cpp")
target_compile_definitions(tls_thread_execution PRIVATE PCSX5_TLS_PATCH_FULL)
include("${CMAKE_CURRENT_SOURCE_DIR}/trace-tests.cmake")
add_test(NAME dispatcher_trace_repeatability COMMAND "${CMAKE_COMMAND}"
    "-DFIXTURE=$<TARGET_FILE:dispatcher_execution>"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/runs/dispatcher_trace"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/dispatcher-trace-tests.cmake")
set_tests_properties(dispatcher_trace_repeatability PROPERTIES
    TIMEOUT 30 LABELS "legacy;characterization;trace")
add_characterization(syscall_dispatch characterization/syscall_dispatch.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/syscall_unreachable_dependencies.cpp"
    "${legacy_root}/src/kernel/syscalls.cpp")
target_compile_definitions(syscall_dispatch PRIVATE PCSX5_SYSCALL_TABLE_CHARACTERIZATION_ONLY)
target_compile_options(syscall_dispatch PRIVATE /Gy)
target_link_options(syscall_dispatch PRIVATE /OPT:REF /INCREMENTAL:NO)
add_characterization(register_context characterization/register_context.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/cpu_unreachable_dependencies.cpp"
    "${legacy_root}/src/cpu/cpu.cpp")
target_compile_definitions(register_context PRIVATE PCSX5_CONTEXT_CHARACTERIZATION_ONLY)
target_compile_options(register_context PRIVATE /Gy)
target_link_options(register_context PRIVATE /OPT:REF /INCREMENTAL:NO)

add_characterization(guest_lifecycle_execution characterization/guest_lifecycle_execution.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_lifecycle_fixture.asm"
    "${legacy_root}/src/kernel/guest_execution.cpp"
    "${legacy_root}/src/hle/guest_lifecycle.cpp"
    "${legacy_root}/src/hle/dispatcher.asm")
add_characterization(lifecycle_state characterization/lifecycle_state.cpp
    "${legacy_root}/src/hle/guest_lifecycle.cpp")

add_executable(kernel_trap kernel_trap.cpp kernel_trap_dependencies.cpp
    "${legacy_root}/src/kernel/kernel.cpp"
    "${legacy_root}/src/kernel/syscalls.cpp"
    "${legacy_root}/src/kernel/tls.cpp"
    "${legacy_root}/src/kernel/tls_patch.cpp"
    "${legacy_root}/src/kernel/guest_tracer.cpp"
    "${legacy_root}/src/kernel/instr_decode.cpp"
    "${legacy_root}/src/cpu/amd_compat.cpp"
    "${legacy_root}/src/hle/guest_lifecycle.cpp")
target_link_libraries(kernel_trap PRIVATE legacy_memory)
target_compile_definitions(kernel_trap PRIVATE PCSX5_TLS_PATCH_FULL PCSX5_KERNEL_TRAP_CHARACTERIZATION_ONLY)
target_compile_options(kernel_trap PRIVATE /W4 /Gy)
target_link_options(kernel_trap PRIVATE /OPT:REF /INCREMENTAL:NO)
foreach(route syscall_guest syscall_host context breakpoint_guest breakpoint_host
        breakpoint_inproc_untracked breakpoint_inproc_tracked fault_guest fault_host_inproc exit_hook)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runs/kernel_trap_${route}")
    add_test(NAME kernel_trap_${route} COMMAND kernel_trap ${route})
    set_tests_properties(kernel_trap_${route} PROPERTIES TIMEOUT 30 LABELS "legacy;characterization;trap"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runs/kernel_trap_${route}")
endforeach()

add_characterization(guest_lifecycle_kernel characterization/guest_lifecycle_execution.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_lifecycle_fixture.asm"
    "${CMAKE_CURRENT_SOURCE_DIR}/kernel_trap_dependencies.cpp"
    "${legacy_root}/src/kernel/guest_execution.cpp"
    "${legacy_root}/src/hle/dispatcher.asm"
    "${legacy_root}/src/kernel/kernel.cpp"
    "${legacy_root}/src/kernel/syscalls.cpp"
    "${legacy_root}/src/kernel/tls.cpp"
    "${legacy_root}/src/kernel/tls_patch.cpp"
    "${legacy_root}/src/kernel/guest_tracer.cpp"
    "${legacy_root}/src/kernel/instr_decode.cpp"
    "${legacy_root}/src/cpu/amd_compat.cpp"
    "${legacy_root}/src/hle/guest_lifecycle.cpp")
target_compile_definitions(guest_lifecycle_kernel PRIVATE PCSX5_TLS_PATCH_FULL
    PCSX5_KERNEL_TRAP_CHARACTERIZATION_ONLY PCSX5_WITH_REAL_KERNEL_VEH)
target_compile_options(guest_lifecycle_kernel PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/Gy>")
target_link_options(guest_lifecycle_kernel PRIVATE /OPT:REF /INCREMENTAL:NO)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runs/sys_exit_child")
add_test(NAME kernel_sys_exit_process COMMAND "${CMAKE_COMMAND}"
    "-DFIXTURE=$<TARGET_FILE:kernel_trap>"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/runs/sys_exit_child"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-process-exit.cmake")
set_tests_properties(kernel_sys_exit_process PROPERTIES TIMEOUT 15 LABELS "legacy;characterization;lifecycle")
add_test(NAME lifecycle_trace_repeatability COMMAND "${CMAKE_COMMAND}"
    "-DFIXTURE=$<TARGET_FILE:guest_lifecycle_execution>"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/runs/lifecycle_trace"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/dispatcher-trace-tests.cmake")
set_tests_properties(lifecycle_trace_repeatability PROPERTIES TIMEOUT 30 LABELS "legacy;characterization;trace")
add_test(NAME combined_lifecycle_trace_repeatability COMMAND "${CMAKE_COMMAND}"
    "-DFIXTURE=$<TARGET_FILE:guest_lifecycle_kernel>"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/runs/combined_lifecycle_trace"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/dispatcher-trace-tests.cmake")
set_tests_properties(combined_lifecycle_trace_repeatability PROPERTIES TIMEOUT 30 LABELS "legacy;characterization;trace")
foreach(route syscall_guest context fault_guest fault_host_inproc)
    add_test(NAME kernel_trace_${route} COMMAND "${CMAKE_COMMAND}"
        "-DFIXTURE=$<TARGET_FILE:kernel_trap>" "-DROUTE=${route}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/runs/kernel_trace_${route}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/dispatcher-trace-tests.cmake")
    set_tests_properties(kernel_trace_${route} PROPERTIES TIMEOUT 30 LABELS "legacy;characterization;trace")
endforeach()
