# CMake toolchain: Zig's Clang targeting x86-64 Windows (MinGW-w64 ABI). Needs the project venv (.venv) with the
# `ziglang` package. Used because this machine's Visual Studio has no Windows SDK, so MSVC cannot link.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(_tc "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_C_COMPILER "${_tc}/zig-cc.cmd")
set(CMAKE_C_COMPILER_TARGET x86_64-windows-gnu)
set(CMAKE_CXX_COMPILER "${_tc}/zig-cxx.cmd")
set(CMAKE_CXX_COMPILER_TARGET x86_64-windows-gnu)
set(CMAKE_AR "${_tc}/zig-ar.cmd")
set(CMAKE_RANLIB "${_tc}/zig-ranlib.cmd")
