@echo off
cd /d "%~dp0"
windres -O coff ai-helper.rc -o ai-helper-res.o
if errorlevel 1 goto fail
gcc -O2 -Wall -municode -mwindows ai-helper.c ai-helper-res.o -o ai-helper.exe -lws2_32 -lshell32 -lcomdlg32 -ladvapi32 -luser32 -lcomctl32 -lole32 -luuid
if errorlevel 1 goto fail
del ai-helper-res.o
echo Built ai-helper.exe
exit /b 0
:fail
echo Build failed.
exit /b 1
