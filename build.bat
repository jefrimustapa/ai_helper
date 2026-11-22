@echo off
cd /d "%~dp0"
where gcc >nul 2>nul
if errorlevel 1 (
    if exist "%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe" (
        set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin;%PATH%"
    )
)
windres -O coff ai-helper.rc -o ai-helper-res.o
if errorlevel 1 goto fail
gcc -O2 -Wall -municode -mwindows ai-helper.c ai-helper-res.o -o ai-helper.exe -lws2_32 -lshell32 -lcomdlg32 -ladvapi32 -luser32 -lcomctl32 -lole32 -luuid
if errorlevel 1 goto fail
del ai-helper-res.o
echo Built ai-helper.exe
exit /b 0
:fail
echo Build failed.
del ai-helper-res.o 2>nul
exit /b 1
