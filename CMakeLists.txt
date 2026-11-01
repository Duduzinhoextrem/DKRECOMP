cmake_minimum_required(VERSION 3.24)
project(DKRecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  # FMA in hardware for FMADD, no unrequested contraction, sqrt without errno (see runtime/include/dkr/fp.h).
  add_compile_options(-Wall -mfma -ffp-contract=off -fno-math-errno)
endif()

# Runtime: what recompiled code links against.
add_library(dkr_runtime STATIC runtime/src/core.c)
target_include_directories(dkr_runtime PUBLIC runtime/include)
if(WIN32)
  target_sources(dkr_runtime PRIVATE runtime/src/shared.c)
endif()

# Known-answer tests of the CPU helpers in runtime/include/dkr/cpu.h. No game files.
add_executable(dkr_selftest tests/native/selftest.c)
target_link_libraries(dkr_selftest PRIVATE dkr_runtime)

# IPC message format (runtime/src/ipc.c) against hand-built messages, including the one the game sends first.
add_executable(dkr_ipc_test tests/native/ipc_test.c runtime/src/ipc.c)
target_link_libraries(dkr_ipc_test PRIVATE dkr_runtime)

if(WIN32)
  # Exercise the actual kernel and event objects with synthetic guest entry points.
  add_executable(dkr_kernel_test tests/native/kernel_test.c runtime/src/kernel.c runtime/src/ipc.c)
  target_link_libraries(dkr_kernel_test PRIVATE dkr_runtime)
  add_executable(dkr_fs_test tests/native/fs_test.c runtime/src/fs.c runtime/src/save.c runtime/src/ipc.c runtime/src/services.c runtime/src/hid.c runtime/src/vi.c runtime/src/audio.c)
  target_link_libraries(dkr_fs_test PRIVATE dkr_runtime)
  add_executable(dkr_shared_test tests/native/shared_test.c runtime/src/hid.c runtime/src/ipc.c)
  target_link_libraries(dkr_shared_test PRIVATE dkr_runtime)
  add_executable(dkr_vi_test tests/native/vi_test.c runtime/src/vi.c runtime/src/ipc.c)
  target_link_libraries(dkr_vi_test PRIVATE dkr_runtime)
  add_executable(dkr_audio_test tests/native/audio_test.c runtime/src/audio.c runtime/src/audio_pcm.c runtime/src/ipc.c)
  target_link_libraries(dkr_audio_test PRIVATE dkr_runtime)
endif()

# Block-linear texture storage sizes against original-SDK queries (runtime/gfx/nvn_tex.c).
add_executable(dkr_nvn_tex_test tests/native/nvn_tex_test.c runtime/gfx/nvn_tex.c)

enable_testing()
add_test(NAME cpu_helpers COMMAND dkr_selftest)
add_test(NAME ipc_format COMMAND dkr_ipc_test)
add_test(NAME nvn_texture_layout COMMAND dkr_nvn_tex_test)
if(WIN32)
  add_test(NAME kernel_sync COMMAND dkr_kernel_test)
  set_tests_properties(kernel_sync PROPERTIES TIMEOUT 30)
  add_test(NAME romfs_storage COMMAND dkr_fs_test)
  add_test(NAME shared_memory COMMAND dkr_shared_test)
  add_test(NAME display_service COMMAND dkr_vi_test)
  add_test(NAME audio_renderer COMMAND dkr_audio_test)
endif()

# The optional host graphics target uses only pinned headers and the Windows Vulkan loader, no system SDK.
option(DKR_BUILD_GFX "Build the Vulkan presentation backend and GPU smoke test" OFF)
if(DKR_BUILD_GFX)
  if(NOT WIN32)
    message(FATAL_ERROR "The current graphics window backend targets Windows")
  endif()
  set(DKR_VULKAN_HEADERS "${CMAKE_SOURCE_DIR}/toolchain/vulkan-headers/include" CACHE PATH "Vulkan header directory")
  if(NOT EXISTS "${DKR_VULKAN_HEADERS}/vulkan/vulkan.h")
    message(FATAL_ERROR "Run .venv/Scripts/python tools/fetch_gfx_deps.py first")
  endif()
  add_library(dkr_gfx STATIC runtime/gfx/vk.c)
  target_include_directories(dkr_gfx PUBLIC runtime/include PRIVATE "${DKR_VULKAN_HEADERS}")
  target_link_libraries(dkr_gfx PUBLIC user32 gdi32)
  add_executable(dkr_gfx_smoke tests/native/gfx_smoke.c)
  target_link_libraries(dkr_gfx_smoke PRIVATE dkr_gfx)
  add_executable(dkr_nvn_test tests/native/nvn_test.c runtime/gfx/nvn.c runtime/gfx/nvn_obj.c runtime/gfx/nvn_tex.c runtime/gfx/nvn_commands.c runtime/gfx/nvn_shaders.c)
  target_link_libraries(dkr_nvn_test PRIVATE dkr_runtime dkr_gfx bcrypt)
  add_test(NAME nvn_entry_abi COMMAND dkr_nvn_test)
  add_test(NAME vulkan_present COMMAND dkr_gfx_smoke --hidden --capture "${CMAKE_BINARY_DIR}/gfx-smoke.bmp")
  set_tests_properties(vulkan_present PROPERTIES TIMEOUT 30)
  set(DKR_VK_VALIDATION_DIR "${CMAKE_SOURCE_DIR}/toolchain/vulkan-validation" CACHE PATH "Optional local Vulkan validation layer")
  if(EXISTS "${DKR_VK_VALIDATION_DIR}/VkLayer_khronos_validation.json")
    add_test(NAME vulkan_validation COMMAND dkr_gfx_smoke --hidden --validation --frames 32
      --capture "${CMAKE_BINARY_DIR}/gfx-smoke-validation.bmp")
    set_tests_properties(vulkan_validation PROPERTIES TIMEOUT 30 ENVIRONMENT
      "VK_LAYER_PATH=${DKR_VK_VALIDATION_DIR};VK_LOADER_LAYERS_DISABLE=~implicit~;VK_VALIDATION_VALIDATE_SYNC=1")
  endif()
endif()

# Optional: the differential-test harness built around a lifted C file (tests/diff/func_diff.py --msvc).
set(DKR_LIFTED_C "" CACHE FILEPATH "Lifted C file to build into dkr_harness")
if(DKR_LIFTED_C)
  add_executable(dkr_harness tests/diff/harness.c ${DKR_LIFTED_C})
  target_link_libraries(dkr_harness PRIVATE dkr_runtime)
endif()
