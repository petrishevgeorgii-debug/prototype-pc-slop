# SPDX-License-Identifier: GPL-2.0-or-later
# Manhattan Engine (docs/ENGINE.md). Included from gpu/CMakeLists.txt, which finds Vulkan, SDL3,
# the Vulkan Memory Allocator, miniz and glslang; built on request (ninja manhattan).

find_package(Threads REQUIRED)

set(ENGINE_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(ENGINE_SHADERS "")
foreach(stage vert frag)
    set(out ${ENGINE_SHADER_DIR}/world_${stage}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ENGINE_SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} -V --target-env vulkan1.2 --vn world_${stage} -o ${out}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/world.${stage}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/world.${stage} VERBATIM)
    list(APPEND ENGINE_SHADERS ${out})
endforeach()

add_executable(manhattan EXCLUDE_FROM_ALL
    src/main.cpp src/p3d.cpp src/city.cpp src/renderer.cpp src/vma.cpp ${ENGINE_SHADERS})
set_target_properties(manhattan PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON INTERPROCEDURAL_OPTIMIZATION OFF)
target_include_directories(manhattan PRIVATE src ${ENGINE_SHADER_DIR})
target_compile_options(manhattan PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(manhattan PRIVATE Vulkan::Vulkan PkgConfig::SDL3 GPUOpen::VulkanMemoryAllocator miniz::miniz
    Threads::Threads)
