@echo off
rem WinExec MCP pack: compile C -> refresh bundled files -> build vsix
rem NOTE: bump VER together with extension\package.json version
setlocal
cd /d "%~dp0"
set VER=0.3.1

rem ---- locate mingw gcc (first arg overrides; else PATH; else common installs) ----
set GCC=%1
if "%GCC%"=="" for %%g in (x86_64-w64-mingw32-gcc gcc) do (
    if not defined GCC where %%g >nul 2>nul && set GCC=%%g
)
if "%GCC%"=="" if exist "D:\Strawberry\c\bin\x86_64-w64-mingw32-gcc.exe" set GCC=D:\Strawberry\c\bin\x86_64-w64-mingw32-gcc.exe
if "%GCC%"=="" if exist "C:\msys64\mingw64\bin\x86_64-w64-mingw32-gcc.exe" set GCC=C:\msys64\mingw64\bin\x86_64-w64-mingw32-gcc.exe
if "%GCC%"=="" if exist "C:\mingw64\bin\gcc.exe" set GCC=C:\mingw64\bin\gcc.exe
if "%GCC%"=="" (
    echo [ERROR] mingw gcc not found. Install Strawberry Perl or MSYS2, or pass path: build.cmd C:\path\to\gcc.exe
    exit /b 1
)
echo using GCC: %GCC%

if not exist bin mkdir bin
"%GCC%" -O2 -static -Wall -o bin\win-exec-mcp.exe win-exec-mcp.c -lws2_32
if errorlevel 1 (echo compile failed & exit /b 1)

if not exist extension\bin mkdir extension\bin
if not exist extension\src mkdir extension\src
copy /Y bin\win-exec-mcp.exe extension\bin\ >nul
copy /Y win-exec-mcp.c extension\src\ >nul

if not exist vsix mkdir vsix
if not exist dist mkdir dist
if exist vsix-build rmdir /s /q vsix-build
mkdir vsix-build
xcopy /e /i /y extension vsix-build\extension\ >nul
copy /Y vsix\[Content_Types].xml vsix-build\ >nul
copy /Y vsix\extension.vsixmanifest vsix-build\ >nul
tar --format zip -cf dist\win-exec-mcp-%VER%.vsix -C vsix-build [Content_Types].xml extension.vsixmanifest extension
if exist dist\win-exec-mcp-%VER%.vsix (echo OK: dist\win-exec-mcp-%VER%.vsix) else (echo vsix failed & exit /b 1)

rem ---- auto install ----
where code >nul 2>nul
if errorlevel 1 (
    echo [WARN] code CLI not found - install manually: dist\win-exec-mcp-%VER%.vsix
) else (
    code --install-extension dist\win-exec-mcp-%VER%.vsix --force
    if errorlevel 1 (echo install failed & exit /b 1) else (echo INSTALLED: win-exec-mcp-%VER% - Reload Window to activate)
)
