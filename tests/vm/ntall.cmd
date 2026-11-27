@echo off
rem ntall.cmd MODE: nt.cmd's whole set in four lanes at once. Tests that need
rem the focus, the foreground or the clipboard go one after another in lane 1;
rem the others share lanes 2-4 by the times of 2026-11-22. Then
rem D:\nt\MODE-done.txt and D:\NTDONE.TXT, for tools/vm-test.py --until-off
rem to shut the VM down. Each lane's unit start times: D:\nt\MODE-times-k.txt.
setlocal
set MODE=%1
rem one run per boot (vm-test.py may type the command again); D:\NTSTART.TXT says it started
if exist %TEMP%\nt-started exit /b
echo %MODE%> %TEMP%\nt-started
echo %MODE%> D:\NTSTART.TXT
if not exist D:\nt mkdir D:\nt
del /q D:\nt\%MODE%-done*.txt 2> nul
start "" /b cmd /c "set NT_LANE=1&& D:\nt.cmd %MODE% user32 comctl32 shell32 ole32"
start "" /b cmd /c "set NT_LANE=2&& D:\nt.cmd %MODE% ntdll advapi32 rpcrt4 setupapi win32u"
start "" /b cmd /c "set NT_LANE=3&& D:\nt.cmd %MODE% kernel32 kernelbase iphlpapi winhttp psapi"
start "" /b cmd /c "set NT_LANE=4&& D:\nt.cmd %MODE% ws2_32 msvcrt ucrtbase gdi32 dbghelp secur32"
:wait
timeout /t 5 /nobreak > nul
for %%k in (1 2 3 4) do if not exist D:\nt\%MODE%-done-%%k.txt goto wait
echo done > D:\nt\%MODE%-done.txt
echo %MODE%> D:\NTDONE.TXT
