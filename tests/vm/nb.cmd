@echo off
rem nb.cmd MODE test:unit ...: ntbench to D:\nt\bench-MODE.txt, then nt.cmd
rem MODE with the units (it ends with D:\NTDONE.TXT). For the same run under
rem ntos and under wineserver (arctic.ntos=0), one VM after the other.
rem D:\NTSTART.TXT says it started (vm-test.py types the command again if it
rem does not come); %TEMP% (a fresh stick each run) keeps a second one out.
if exist %TEMP%\nt-started exit /b
echo %1> %TEMP%\nt-started
echo %1> D:\NTSTART.TXT
if not exist D:\nt mkdir D:\nt
D:\ntbench.exe %1 > D:\nt\bench-%1.txt 2>&1
call D:\nt.cmd %*
