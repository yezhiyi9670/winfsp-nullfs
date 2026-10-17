@echo off
rem ---------------------------------------------------------------------------
rem  Build NullFS with the Microsoft compiler.
rem
rem  Run this from a Visual Studio developer command prompt, e.g.
rem  "x64 Native Tools Command Prompt for VS 2022".
rem
rem      build.bat            (builds x64)
rem      build.bat x86        (builds x86)
rem
rem  If WinFsp is not installed in the default location, set WINFSP_ROOT first:
rem      set WINFSP_ROOT=D:\tools\WinFsp
rem ---------------------------------------------------------------------------

setlocal

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"

if not "%WINFSP_ROOT%"=="" goto have_root
set "WINFSP_ROOT=%ProgramFiles(x86)%\WinFsp"
:have_root

if exist "%WINFSP_ROOT%\inc\winfsp\winfsp.h" goto have_inc
echo ERROR: WinFsp developer files not found under "%WINFSP_ROOT%".
echo        Install WinFsp v2.1 with the "Developer" feature selected, or set
echo        WINFSP_ROOT to the WinFsp install directory.
exit /b 1
:have_inc

where cl >nul 2>nul
if errorlevel 1 goto no_cl

echo Building nullfs-%ARCH%.exe with WinFsp from "%WINFSP_ROOT%"
echo.

cl /nologo /W3 /O2 /MD /EHsc ^
    /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS ^
    /I"%WINFSP_ROOT%\inc" ^
    nullfs.cpp nullfs-main.c ^
    /Fe:nullfs-%ARCH%.exe ^
    /link /LIBPATH:"%WINFSP_ROOT%\lib" winfsp-%ARCH%.lib advapi32.lib delayimp.lib ^
    /DELAYLOAD:winfsp-%ARCH%.dll
if errorlevel 1 goto failed

echo.
echo Built nullfs-%ARCH%.exe
exit /b 0

:no_cl
echo ERROR: cl.exe was not found on the PATH.
echo        Open a "Native Tools Command Prompt for VS" and run this script again.
exit /b 1

:failed
echo.
echo Build failed.
exit /b 1
