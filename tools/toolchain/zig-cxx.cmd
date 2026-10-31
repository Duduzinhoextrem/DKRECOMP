@echo off
rem Runs Zig's C++ compiler from the project venv, without a system Windows SDK.
"%~dp0..\..\.venv\Scripts\python.exe" -m ziglang c++ %*
