@echo off
rem ut.cmd MODE [units...]: every user32 unit to D:\ut\MODE-unit.txt, then D:\ut\MODE-done.txt
set MODE=%1
shift
set UNITS=
:args
if "%1"=="" goto run
set UNITS=%UNITS% %1
shift
goto args
:run
if "%UNITS%"=="" set UNITS=class wsprintf resource generated testdll broadcast clipboard winstation dce dde monitor uitools text static scroll combo listbox edit dialog menu cursoricon sysparams input win msg
if not exist D:\ut mkdir D:\ut
for %%u in (%UNITS%) do (
  echo %%u > D:\ut\%MODE%-cur.txt
  D:\user32_test.exe %%u > D:\ut\%MODE%-%%u.txt 2>&1
)
echo done > D:\ut\%MODE%-done.txt
