@echo off
REM ============================================================
REM  soku_config.exe  --  standalone x86 build
REM  Run from project root:  build_gui.cmd
REM
REM  ASCII-only + CRLF on purpose.  cmd.exe reads .cmd as OEM/ANSI, so a
REM  Chinese path literal (D:\<cjk>\...) would be mangled.  MSVC and the
REM  SDK are probed instead of hardcoded.
REM ============================================================
setlocal enabledelayedexpansion

set "VCDIR="
for %%R in (C D E) do (
  for /d %%A in ("%%R:\*") do (
    if exist "%%A\VC\Tools\MSVC" (
      for /d %%B in ("%%A\VC\Tools\MSVC\*") do (
        if exist "%%B\bin\Hostx86\x86\cl.exe" set "VCDIR=%%B"
      )
    )
  )
)
if not defined VCDIR (echo [FAIL] MSVC cl.exe not found & exit /b 1)
echo MSVC: %VCDIR%

set "SDK="
for %%S in ("%ProgramFiles(x86)%\Windows Kits\10" "D:\Windows Kits\10" "C:\Windows Kits\10" "E:\Windows Kits\10") do (
  if exist "%%~S\Include" set "SDK=%%~S"
)
if not defined SDK (echo [FAIL] Windows SDK not found & exit /b 1)

set "SV="
for /d %%V in ("%SDK%\Include\10.*") do set "SV=%%~nxV"
if not defined SV (echo [FAIL] no SDK version & exit /b 1)
echo SDK : %SDK%  ver %SV%

set "INCLUDE=%VCDIR%\include;%SDK%\Include\%SV%\um;%SDK%\Include\%SV%\shared;%SDK%\Include\%SV%\ucrt"
set "LIB=%VCDIR%\lib\x86;%SDK%\Lib\%SV%\ucrt\x86;%SDK%\Lib\%SV%\um\x86"
REM SDK\bin holds mt.exe, needed to embed the GUI manifest (else LNK1158).
set "PATH=%VCDIR%\bin\Hostx86\x86;%SDK%\bin\%SV%\x86;%PATH%"

cd /d "%~dp0"
if not exist out mkdir out

"%VCDIR%\bin\Hostx86\x86\cl.exe" /nologo /source-charset:utf-8 /execution-charset:gbk /O2 /MT /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /EHsc loader\tools\soku_config.cpp /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:loader\tools\soku_config.manifest /OUT:out\soku_config.exe /MACHINE:X86 comctl32.lib user32.lib gdi32.lib gdiplus.lib uxtheme.lib ole32.lib advapi32.lib

if exist out\soku_config.exe (echo [OK] out\soku_config.exe) else (echo [FAIL] build failed)
endlocal
