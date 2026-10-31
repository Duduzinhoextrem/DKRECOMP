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

# Known-answer tests of the CPU helpers in runtime/include/dkr/cpu.h. No game files.
add_executable(dkr_selftest tests/native/selftest.c)
target_link_libraries(dkr_selftest PRIVATE dkr_runtime)

# IPC message format (runtime/src/ipc.c) against hand-built messages, including the one the game sends first.
add_executable(dkr_ipc_test tests/native/ipc_test.c runtime/src/ipc.c)
target_link_libraries(dkr_ipc_test PRIVATE dkr_runtime)

enable_testing()
add_test(NAME cpu_helpers COMMAND dkr_selftest)
add_test(NAME ipc_format COMMAND dkr_ipc_test)

# Optional: the differential-test harness built around a lifted C file (tests/diff/func_diff.py --msvc).
set(DKR_LIFTED_C "" CACHE FILEPATH "Lifted C file to build into dkr_harness")
if(DKR_LIFTED_C)
  add_executable(dkr_harness tests/diff/harness.c ${DKR_LIFTED_C})
  target_link_libraries(dkr_harness PRIVATE dkr_runtime)
endif()
