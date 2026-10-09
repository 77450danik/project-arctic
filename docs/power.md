# Живлення ноутбука: акумулятор, схеми, відеокарти (7.10.2026)

Користувач: «пакет ноутбучний — batmeter.dll, відсотки в треї з
вспливашкою; налаштування живлення і планів живлення в Панелі керування;
красива панель з відсотками, ватами, відеокартою; вибір відеокарти для
кожної програми, як у Windows. 1 в 1 як у Windows 10, без єдиної
відмінності»; «налаштування мають однаково добре працювати на всіх ноутах
і бути універсальними».

## Рішення користувача

- Еталон — Windows 10 22H2 на E: (розмітка, тексти uk-UA, гліфи).
- Повзунок «Режим живлення» у флайауті справжній (CPU, GPU).
- «Параметри живлення від акумулятора» → Панель керування → Електроживлення → сторінка «Акумулятор» (з 2026-10-09; було — головна сторінка «Електроживлення»).
- Нова сторінка «Акумулятор» (основна з трею: посилання флайаута, перший пункт меню жирним, подвійний клік): %, стан, час; вати + графік за
  годину; знос (проєктна/фактична ємність, цикли); відеокарти та програми
  на них.
- «Дозволити Windows вирішувати»: від мережі — дискретна, від акумулятора —
  вбудована.
- Схеми діють по-справжньому: екран (вимкнення, яскравість, приглушення),
  кришка/кнопки, критичний рівень, процесор, PCI/USB/Wi-Fi, «Економія
  заряду».
- Сон (suspend-to-RAM) — у цьому ж пакеті.

## Універсальність

Нічого не прив'язано до одного ноутбука. Під час роботи визначається:

| Що | Звідки | Немає — |
|---|---|---|
| акумулятори (0…N) | `/sys/class/power_supply`, `type=Battery`, `scope≠Device`; `energy_*` або `charge_*`×`voltage_min_design` | значка немає, як на ПК; одна колонка в схемах |
| мережа | `type=Mains`/`USB*`/`Wireless`, `online` | за станом акумуляторів |
| потужність | `power_now` або `current_now`×`voltage_now`; час — зі згладженої (≈1 хв) | «—» |
| процесор | `scaling_driver`: intel_pstate (EPP 0–255) / amd-pstate (EPP-назви) / інші (governor, `scaling_max_freq`) | — |
| турбо | `intel_pstate/no_turbo` або `cpufreq/boost` | — |
| профіль платформи | `/sys/firmware/acpi/platform_profile` (low-power/quiet/cool, balanced, performance) | пропускається |
| PCIe ASPM | `/sys/module/pcie_aspm/parameters/policy` | прошивка тримає собі — пропуск |
| USB | `power/control` пристроїв, крім HID і хабів | — |
| підсвітка | `/sys/class/backlight`: firmware > platform > raw | повзунків яскравості немає |
| відеокарти | `/sys/class/drm/card*` (PCI, `boot_vga`), hwmon, RAPL `uncore` для Intel, NVML для NVIDIA | одна карта → вибору немає |
| програми на карті | `/proc/*/fdinfo` (DRM: час рушіїв, пам'ять), NVML; інакше `/dev/nvidiaN` | — |
| кришка | `/proc/acpi/button/lid` | рядка «кришка» немає |
| сон | `/sys/power/state` (`mem`), `mem_sleep` (deep/s2idle) | пункту «Сон» немає |

## Частини

| Що | Де |
|---|---|
| ntdll/kernel32: `SystemBatteryState`, `GetSystemPowerStatus` — усі акумулятори, `SystemStatusFlag` = економія заряду | Wine-патч 0074 |
| powrprof.dll: справжній API схем Windows (Power*), накладки повзунка, `SetSuspendState`; unix-частина — залізо (`Arctic*` експорти) | модуль `dlls/powrprof` (замість патча 0069) |
| визначення параметрів і схем Windows 10, назви uk-UA | `runtime/registry/power.reg`, `dlls/powrprof/powrprof.rc` ← `tools/power/win10-power.py` |
| політика живлення: застосування схеми, бездіяльність, кришка/кнопки, рівні заряду, економія заряду, історія, змінні відеокарт | `programs/winlogon/power.c` |
| запити програм (відео не гасить екран): `SetThreadExecutionState`, `PowerSetRequest` | Wine-патч 0075 |
| відеокарта програми при запуску | Wine-патч 0076 (kernelbase `CreateProcess`) |
| значок, підказка, флайаут, меню, сповіщення | модуль `dlls/batmeter`; гліфи ← `tools/power/mdl2-glyphs.py` |
| stobject: служба живлення створює batmeter | ReactOS-патч 0053; `SysTray\Services` = 7 |
| «Електроживлення» в Панелі керування (папка оболонки) і сторінки | модуль `dlls/powercpl` |
| `control powercfg.cpl` | модуль `dlls/powercfg.cpl`; ReactOS-патч 0054 (`don't load`) |
| меню «Пуск»: «Сон», «Режим глибокого сну» за `FlyoutMenuSettings` | startui `menu.c` |
| хост: сон у пам'ять (NVIDIA suspend/resume, годинник), Wi-Fi power save (nl80211), ручки sysfs для `nt` | `host/init/arctic-init.c`, `99-arctic.rules` |
| гасіння екрана | dwm: `ArcticDisplay\MonitorsOff` (volatile) → CRTC вимкнено, буфери повернуто; увімкнення — modeset на власний кадр і кадр з нуля. `ArcticDisplay\Relight` (csrss, Ctrl+Alt+Del двічі) — те саме для ввімкненого екрана |

### Політика (winlogon)

- Ефективне значення параметра = накладка повзунка (лише над
  «Збалансованою»: `ActiveOverlayAc/DcPowerScheme`, `ProvAc/DcSettingIndex`)
  → схема (`ACSettingIndex` користувача → `AcSettingIndex` Windows) →
  для схеми користувача — схема-основа (`ArcticBaseScheme`).
- Застосовується при зміні джерела, схеми, повзунка, економії заряду
  (стежить за `HKLM\...\Control\Power`): EPP (Windows 0–100), мін/макс стан
  процесора, турбо (`PERFBOOSTMODE`), профіль платформи (з EPP і «Політики
  охолодження»), ASPM, USB, яскравість, Wi-Fi power save.
- Економія заряду: на акумуляторі нижче `ESBATTTHRESHOLD` (20 %) вмикається
  сама; від мережі вимикається; вимкнена вручну нижче рівня — до наступного
  заряджання; яскравість × `ESBRIGHTNESS` (70 %), EPP 100, без турбо,
  максимум процесора 70 %, Wi-Fi power save. Стан —
  `HKLM\...\Control\Power\ArcticEnergySaverOn` (+`Manual`).
- Бездіяльність (`GetLastInputInfo`, ввід від winsrv `Global\ArcticUserInput`,
  пробудження, «пінги» програм): приглушення (`VIDEODIM`, рівень
  `VIDEODIMLEVEL`; лише коли вимкнення екрана ввімкнено і настає пізніше, як
  у Windows), вимкнення екрана (`VIDEOIDLE`), сон (`STANDBYIDLE`), гібернація
  (`HIBERNATEIDLE`), якщо програма не тримає екран/систему (запити, 0075).
- Схема застосовується наново лише коли змінилося щось із її значень: власні
  записи політики під тим самим ключем (стан екрана, відеокарти, яскравість)
  її не чіпають; яскравість пишеться лише коли потрібне значення інше.
- Журнал рішень у `nt.log`, рядки `winlogon: power:` — затемнення/екран/сон
  з часом без вводу, кришка, кнопки, критичний заряд, кожен запис
  яскравості з причиною, хто з програм тримає екран чи систему.
- Кнопка живлення/сну (winsrv → `ArcticPowerButton`), кришка
  (`/proc/acpi/button/lid`), низький рівень → сповіщення Windows 10 і
  `PBT_APMBATTERYLOW`, резервний → «Акумулятор майже розряджено», критичний
  → дія (гібернація, якщо ввімкнена, інакше завершення роботи).
- Програмам: `WM_POWERBROADCAST` (`PBT_APMPOWERSTATUSCHANGE`, `PBT_APMSUSPEND`,
  `PBT_APMRESUME*`).
- Відеокарти: `HKLM\...\Control\Power\ArcticGpu` (volatile) — набори змінних
  для «енергозберігаючої» і «високопродуктивної» карти: DXVK/vkd3d-фільтри
  за назвою, NVIDIA PRIME offload + шар Optimus (`NVIDIA_only` /
  `non_NVIDIA_only`), Mesa `MESA_VK_DEVICE_SELECT`, `DRI_PRIME`; OpenGL — лише
  для явного вибору «Висока продуктивність». Програми з `C:\Windows` не
  чіпаються.

## Випадки на ноутбуці

**2026-10-09, серйозне: синій екран з курсором після 10–15 хв YouTube на
весь екран.** Усе зникає, лишається колір робочого столу і вказівник,
Ctrl+Alt+Del не діє — довелося вимикати кнопкою. Журнал тієї сесії втрачено:
`host/nt.log` не писалися на диск до кінця, а `kernel.log` наступний запуск
переписав. 10 хв — це «Вимикати дисплей» від мережі у «Збалансованій»; відео
на весь екран dwm показує напряму (direct scanout), а DPMS лишав площину на
буфері Chrome — буфер, звільнений поки екран темний, забирає в ядра і CRTC,
і після «пробудження» видно лише апаратний курсор. Зроблено: гасіння через
вимкнення CRTC з поверненням буферів, увімкнення — modeset і кадр з нуля;
Ctrl+Alt+Del з RIT через подію (не гаряча клавіша) на окремий потік
winlogon, вдруге — наново вмикає монітори; системні процеси NT (wineserver,
dwm, csrss, winlogon, wininit, services, explorer) поза OOM-killer'ом
(arctic-init, `oom_score_adj`); журнали — fsync кожні 30 с, `kernel.log`
ротується. Якщо повториться: `nt.1.log` — `dwm: display`, `winlogon: power:`
(чи тримав Chrome екран: «keeps the display on»), `csrss: Ctrl+Alt+Del`;
`kernel.1.log` — OOM, GPU hang.

**Сон «сам по собі».** На ноутбуці три засинання за сесію, причини не
писалися — тепер кожне в `nt.log` («N s without input: sleep», «lid closed»,
«power button»). Після S3 WSL (qemu-nbd на файлі з F:) давав I/O errors до
перезапуску — тепер arctic-lxss зупиняється перед сном і стартує після.

**Мерехтіння яскравості раз на ~хвилину.** Від мережі за 3 хв запису
`intel_backlight` не змінювався. Прибрано можливі причини: затемнення при
«Вимикати дисплей: ніколи» (`VIDEODIM` 585/285 с лишалися чинними),
повторне застосування схеми й запис яскравості на кожну власну зміну ключа.
Якщо лишиться — рядки `winlogon: power: brightness` у `nt.log` покажуть, хто
пише.

## Джерела Windows 10

- Флайаут: `Windows.UI.PCShell.pri` → XBF `BatteryFlyoutExperience`
  (MainPage 0076, стилі 0077), тексти — `pris/Windows.UI.PCShell.uk-UA.pri`;
  таблиця гліфів — `BatteryFlyoutExperience.dll`. Файли WOF-стиснені:
  `D:\WORK_YT\tmp\xbf\wofcat.sh`; XBF→XAML — `D:\WORK_YT\tmp\xbf\xbf2xaml.ps1`.
- Підказка — `batmeter.dll.mui`; меню й сповіщення — `stobject.dll.mui`;
  значок малює гліф Segoe MDL2 (stobject).
- «Електроживлення» — `powercpl.dll` (UIFILE 101–104, 110; рядки mui),
  «Додаткові настройки» — `powercfg.cpl` (DIALOG 1000).
- Налаштування графіки — `Windows.UI.SettingsAppThreshold.uk-UA.pri`.
- Параметри й схеми — кущ SYSTEM (`ControlSet001\Control\Power`), назви з
  mui powrprof, mshtml, stobject, wlansvc, usbui, wmpnetwk, mfplat, evr,
  batmeter (перенесено в powrprof з 9000).

## Перевірка

VM (KVM у arctic-build, вікно через noVNC у Chrome; `C:\hibtest`):
`arctic.dev=1 arctic.testpower` — модуль `test_power` дає акумулятор і
мережу; через оболонку на COM-порту:
`echo 0 > /sys/module/test_power/parameters/ac_online`,
`echo 15 > .../battery_capacity`, `echo discharging > .../battery_status`.
Потім ноутбук.
