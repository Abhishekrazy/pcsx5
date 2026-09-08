add_library(pcsx5_graphics STATIC src/renderer.cpp)
target_compile_features(pcsx5_graphics PUBLIC cxx_std_23)
target_include_directories(pcsx5_graphics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
pcsx5_enable_warnings(pcsx5_graphics)

option(PCSX5_BUILD_VULKAN "Build the opt-in Vulkan headless renderer" OFF)
if(PCSX5_BUILD_VULKAN)
    # Installed, explicitly approved tools only: configuration never downloads SDKs.
    find_package(Vulkan 1.1 REQUIRED COMPONENTS glslangValidator)
    find_program(PCSX5_SPIRV_VAL NAMES spirv-val
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
    set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    foreach(stage vert frag)
        add_custom_command(OUTPUT "${shader_dir}/frame.${stage}.spv"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
            COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
                -o "${shader_dir}/frame.${stage}.spv"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame.${stage}"
            COMMAND "${PCSX5_SPIRV_VAL}" --target-env vulkan1.1
                "${shader_dir}/frame.${stage}.spv"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame.${stage}"
            VERBATIM)
    endforeach()
    add_custom_command(OUTPUT "${shader_dir}/frame_shaders.h"
        COMMAND "${CMAKE_COMMAND}" "-DVERT=${shader_dir}/frame.vert.spv"
            "-DFRAG=${shader_dir}/frame.frag.spv"
            "-DOUTPUT=${shader_dir}/frame_shaders.h"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFrameShaders.cmake"
        DEPENDS "${shader_dir}/frame.vert.spv" "${shader_dir}/frame.frag.spv"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedFrameShaders.cmake"
        VERBATIM)
    add_library(pcsx5_graphics_vulkan STATIC src/vulkan_renderer.cpp
        "${shader_dir}/frame_shaders.h")
    target_link_libraries(pcsx5_graphics_vulkan PUBLIC pcsx5_graphics PRIVATE Vulkan::Vulkan)
    target_include_directories(pcsx5_graphics_vulkan PRIVATE "${shader_dir}")
    pcsx5_enable_warnings(pcsx5_graphics_vulkan)
endif()
