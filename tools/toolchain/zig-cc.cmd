@echo off
rem Runs Zig's cc from the project venv (see docs/TESTING.md).
"%~dp0..\..\.venv\Scripts\python.exe" -m ziglang cc %*
