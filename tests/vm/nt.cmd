@echo off
rem nt.cmd MODE [test | test:unit ...]: Wine's conformance tests of what NT's executive
rem serves (docs/ntos.md "Перевірка"), every unit of each to
rem D:\nt\MODE-test-unit.txt, then D:\nt\MODE-done.txt. A unit still running
rem after 3 minutes is stopped and "TIMEOUT" ends its file. MODE: ntos, or
rem wineserver (arctic.ntos=0). Each unit's start time goes to D:\nt\MODE-times.txt.
rem In a lane of ntall.cmd (NT_LANE=k) the files are MODE-cur-k, MODE-times-k
rem and MODE-done-k; a test (its .exe) belongs to one lane only, the watchdog
rem finds it by its name. A run on its own ends with D:\NTDONE.TXT, for
rem tools/vm-test.py --until-off to shut the VM down.
setlocal enabledelayedexpansion
set MODE=%1
set SFX=
if defined NT_LANE set SFX=-%NT_LANE%
shift
set TESTS=
:args
if "%1"=="" goto run
set TESTS=%TESTS% %1
shift
goto args
:run
rem on its own: D:\NTSTART.TXT says it started (vm-test.py --until-off types the
rem command again if not), and one run per boot (%TEMP% is on the VM's fresh stick)
if not defined NT_LANE (
  echo %MODE%> D:\NTSTART.TXT
  if not exist %TEMP%\nt-started echo %MODE%> %TEMP%\nt-started
)
if "%TESTS%"=="" set TESTS=ntdll kernel32 kernelbase advapi32 ws2_32 rpcrt4 ole32 shell32 msvcrt ucrtbase setupapi iphlpapi winhttp comctl32 dbghelp psapi secur32 win32u gdi32 user32
if not exist D:\nt mkdir D:\nt
rem test (every unit of it) or test:unit (that one)
for %%a in (%TESTS%) do (
  for /f "tokens=1,2 delims=:" %%t in ("%%a") do (
    if not exist D:\%%t_test.exe (
      echo missing > D:\nt\%MODE%-%%t.txt
    ) else if not "%%u"=="" (
      call :one %%t %%u
    ) else (
      rem a test given no unit lists them, indented, after "Valid test names:"
      for /f "tokens=1" %%v in ('D:\%%t_test.exe 2^>^&1 ^| find "    "') do call :one %%t %%v
    )
  )
)
echo %time% end >> D:\nt\%MODE%-times%SFX%.txt
echo done > D:\nt\%MODE%-done%SFX%.txt
rem on its own (not a lane): the whole run is done, vm-test.py --until-off stops the VM
if not defined NT_LANE echo %MODE%> D:\NTDONE.TXT
goto :eof

:one
echo %1 %2 > D:\nt\%MODE%-cur%SFX%.txt
echo %time% %1:%2 >> D:\nt\%MODE%-times%SFX%.txt
start "" /b cmd /c "D:\%1_test.exe %2 > D:\nt\%MODE%-%1-%2.txt 2>&1"
set /a N=0
:wait
timeout /t 1 /nobreak > nul
tasklist /fi "imagename eq %1_test.exe" | find /i "%1_test.exe" > nul || goto :eof
set /a N+=1
if !N! lss 180 goto wait
taskkill /f /t /im %1_test.exe > nul 2>&1
echo TIMEOUT >> D:\nt\%MODE%-%1-%2.txt
goto :eof
