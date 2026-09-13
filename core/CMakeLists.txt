add_library(pcsx5_core STATIC
    src/version.cpp
    src/guest_memory.cpp
    src/elf_parser.cpp
    src/loader.cpp
    src/dynamic_info.cpp
    src/relocation.cpp
    src/nid.cpp
    src/hle_registry.cpp
)

target_include_directories(pcsx5_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(pcsx5_core PUBLIC cxx_std_23)
pcsx5_enable_warnings(pcsx5_core)
