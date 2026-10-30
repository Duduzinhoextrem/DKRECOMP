@echo off
rem Runs Zig's ar from the project venv (see docs/TESTING.md).
"%~dp0..\..\.venv\Scripts\python.exe" -m ziglang ar %*
