@echo off
REM ===========================================================================
REM  build_msvc.bat - build the demo with MSVC. Output: build\GlowDemo.exe
REM
REM  Run this from a shell that already has vcvars loaded (vcvars32.bat or
REM  vcvars64.bat), or use CMakeLists.txt instead.
REM
REM  Needs a Dear ImGui checkout. Point IMGUI_DIR at one, or let it default to
REM  third_party\imgui (the git submodule).
REM
REM  NOTE: keep this file ASCII-only. cmd.exe reads .bat byte by byte, so a
REM  UTF-8 multi-byte character shifts the offsets and breaks goto/if blocks.
REM ===========================================================================
setlocal

if "%IMGUI_DIR%"=="" set IMGUI_DIR=%~dp0third_party\imgui

REM %~dp0 ends with a backslash. Strip it: a path like "D:\dir\" inside quotes
REM makes the closing quote an ESCAPED quote to the C runtime's argv parser, so
REM the argument swallows the rest of the command line. cl then receives zero
REM source files and dies with "D8003: missing source file name".
set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

if not exist "%IMGUI_DIR%\imgui.h" ( echo Dear ImGui not found at "%IMGUI_DIR%" - run: git submodule update --init --recursive & exit /b 1 )
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
pushd "%ROOT%"

cl /nologo /O2 /MT /W3 /EHsc /utf-8 /std:c++17 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%" /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
   /Fo"%ROOT%\build\\" /Fd"%ROOT%\build\\" ^
   /Fe"%ROOT%\build\GlowDemo.exe" ^
   example\glow_demo.cpp ^
   "%IMGUI_DIR%\imgui.cpp" "%IMGUI_DIR%\imgui_draw.cpp" "%IMGUI_DIR%\imgui_tables.cpp" "%IMGUI_DIR%\imgui_widgets.cpp" ^
   "%IMGUI_DIR%\backends\imgui_impl_win32.cpp" "%IMGUI_DIR%\backends\imgui_impl_dx11.cpp" ^
   /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO ^
   d3d11.lib dxgi.lib d3dcompiler.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib

set RC=%errorlevel%
popd
if %RC% neq 0 ( echo BUILD FAILED & exit /b %RC% )
echo.
echo === build\GlowDemo.exe ok ===
