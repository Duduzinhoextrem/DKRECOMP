@echo off
rem Runs Zig's ranlib from the project venv (see docs/TESTING.md).
"%~dp0..\..\.venv\Scripts\python.exe" -m ziglang ranlib %*
