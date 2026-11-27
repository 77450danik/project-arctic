# WinUSB

Програми для Windows, що працюють із USB-пристроєм напряму (adb і fastboot
Android, scrcpy через adb, прошивальники з WinUSB), відкривають інтерфейс
пристрою за GUID і говорять із ним через `winusb.dll`. В Arctic це той самий
шлях, що й у Windows:

```
програма → winusb.dll → DeviceIoControl → winusb.sys → URB → windowsusb.sys → libusb → usbfs
```

- **windowsusb.sys** (Wine `wineusb.sys`) — шина: пристрої й інтерфейси
  складених пристроїв (`USB\VID_…&PID_…&MI_xx`), URB. Патч 0151: скасування
  передачі разом з IRP (CancelIoEx, закритий дескриптор, тайм-аут),
  `ABORT_PIPE` лише свого каналу, запити класу, відмова libusb завершує IRP,
  передачі вийнятого пристрою відпускаються до завершення IRP.
- **winusb.sys** (`runtime/wine/modules/dlls/winusb.sys`) — драйвер функції.
  `winusb.inf` ставить його на інтерфейси ADB (`USB\Class_ff&SubClass_42&Prot_01`),
  fastboot (`…&Prot_03`) і `USB\MS_COMP_WINUSB`. Інтерфейси пристрою — з
  `DeviceInterfaceGUIDs` у «Device Parameters» (секція `.HW` .inf), для ADB і
  fastboot без них — `ANDROID_USB_CLASS_ID` `{F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}`,
  як у драйвера Google. Запит програми йде вниз її ж IRP.
- **winusb.dll** (`runtime/wine/modules/dlls/winusb`) — увесь API WinUSB, крім
  ізохронних каналів. Запити до драйвера — власні Arctic
  (`include/wine/arctic_winusb.h`): читання `METHOD_OUT_DIRECT`, запис
  `METHOD_IN_DIRECT`, тож `GetOverlappedResult` дає передані байти.
  `PIPE_TRANSFER_TIMEOUT` — для синхронних викликів.

Нічого не залежить від wineserver: лише IRP, PnP та інтерфейси пристроїв NT.

## Перевірка

1. Телефон: «Параметри розробника» → «Налагодження через USB».
2. Під'єднати, на телефоні дозволити ПК.
3. `adb devices -l` — телефон зі своїм серійним номером, `scrcpy`.

Журнал: `WINEDEBUG=+winusb,+wineusb` перед запуском adb (сервер adb треба
перезапустити: `adb kill-server`), рядки в `C:\Windows\Logs\Arctic\nt.log`.

## Ще немає

- дескрипторів Microsoft OS: шина не дає `USB\MS_COMP_WINUSB`, тож пристрої,
  що просять WinUSB лише ними, драйвера не отримують;
- альтернативних налаштувань інтерфейсу, ізохронних каналів;
- тайм-ауту каналу для перекритих (overlapped) викликів.
