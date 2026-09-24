@echo off
rem WinExec MCP - run fix-firewall.ps1 as admin (self-elevating)
net session >nul 2>&1
if not errorlevel 1 goto elevated
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b
:elevated
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fix-firewall.ps1"
