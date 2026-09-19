# Project Arctic — план гібридної ОС

Ядро і драйвери — Linux. Усе, що бачить користувач і що бачать програми, — Windows.

Дата: 2026-09-19. Статус: архітектуру погоджено, реалізацію не почато.

---

## 0. Суть

Arctic — це не дистрибутив Linux, на якому запускається Wine. Це ОС, де:

- **Linux** — лише ядро, драйвери, прошивки і кілька невидимих бекендів (udevd, iwd, BlueZ).
- **Wine** — не контейнер, а **реалізація NT**. `wineserver` — виконавча система NT, яку стартує init
  і яка живе, поки працює машина. Є один C:, один реєстр і один шлях створення процесу.
- **Екран і ввід належать Windows-світу.** `dwm.exe` сам тримає DRM/KMS і композитить,
  `csrss.exe` сам читає клавіатуру й мишу. Між Windows-програмою і драйвером GPU немає X11,
  немає Wayland-композитора, немає Linux-desktop.
- **Міст** — набір наших модулів, які перекладають стандартні Windows API (ChangeDisplaySettingsEx,
  WASAPI, WLAN API, SetupAPI, …) у виклики драйверів Linux. Панель керування з ReactOS змінює
  роздільність — і перемикається реальний режим KMS.

---

## 1. Інваріанти (порушувати не можна)

1. **Один NT-світ.** Один `wineserver` на машину, один C:, один реєстр. Кожен EXE, від Блокнота до
   D3D12-гри, проходить через `CreateProcess`. Немає префіксів на програму, лаунчерів, «режиму Proton».
2. **Linux невидимий.** Немає диска Z:, немає shell у релізному образі, немає VT-консолі,
   ELF-програм у моделі користувача теж немає.
3. **Політика — у Windows-світі.** Linux-компоненти лише виконують: KMS показує, ALSA грає, iwd асоціюється.
   Рішення (яке вікно зверху, який пристрій за замовчуванням, до якої мережі підключатись) приймає
   Windows-сторона.
4. **Одне джерело правди про вікна** — `wineserver` + `win32u`. `dwm.exe` лише показує.
5. **Залізо доступне тільки через стандартні Windows API** (таблиця в розділі 4). Своїх API для програм немає.
6. **Два графічні режими, один код.** GPU-режим (Vulkan) і базовий (CPU, як XP без DWM).
   Перехід між ними автоматичний.
7. **Кожен етап закінчується ISO, що завантажується** в QEMU і на залізі.

---

## 2. Зафіксовані рішення

| Питання | Рішення | Наслідок |
|---|---|---|
| Дисплей | `dwm.exe` (PE) володіє DRM/KMS, композитить. Буфери між процесами йдуть внутрішнім з'єднанням у форматі Wayland-wire | Mesa і NVIDIA одразу вміють віддавати кадри без копій; Wayland ніде не видно, а ввід через нього не йде |
| Ввід | `csrss.exe`: libinput → `wineserver` напряму | Хіт-тест, фокус, захоплення миші — як у win32k, без обмежень Wayland |
| Рантайм | Upstream Wine 11.x (dev-гілка) + компоненти Proton | Наші зміни не конфліктують з річними ребейзами Valve |
| 32-біт | Лише новий WoW64 з Wine 11 | Жодної 32-бітної Linux-бібліотеки; `lib32`-зоопарк старої Arctic зникає |
| Ядро | Чисте mainline (зараз 7.2), свій конфіг, без патчів | Оновлення = зміна версії |
| NVIDIA | І nvidia-open, і nouveau+NVK, автовибір | RTX 20xx+ → NVIDIA (швидше, DLSS), старші → nouveau |
| Звук | Свій аудіорушій: `Audiosrv` + `audiodg.exe` → ALSA hw | Без PipeWire/PulseAudio/dmix |
| Linux-бекенди | udevd, iwd, BlueZ + D-Bus | CUPS не беремо: друк через Windows-спулер напряму по IPP |
| Користувачі | Один, без екрана входу | `winlogon.exe` є (Ctrl+Alt+Del, завершення роботи), але автологін |
| Версія для програм | Windows 11 25H2 (10.0.26200) | «Режим сумісності» у властивостях EXE для старих програм |
| Вигляд | Класичний макет ReactOS explorer + visual style **Mizu** (`reactos/media/themes/Mizu`) | Схожий на Windows 10, без нової оболонки |
| C: | NTFS (новий драйвер ядра 7.1, `nocase=1`) — і в живому режимі | Флешка читається на будь-якому Windows-ПК, там же можна запустити `chkdsk`. Журналювання в драйвері поки немає — див. розділ 10 |
| Поставка | Поки лише LiveUSB | Інсталятор — після v0.1 |
| Зі старої Arctic | Українська мова, EN/UA/RU + Alt+Shift, BIOS + UEFI без Secure Boot, збереження на флешку окремим пунктом меню | Пункту «Налагодження» з Linux-консоллю **немає** |
| Античит | Після бази (v0.2) | Міст EAC/BattlEye з Proton. Ядрові античити неможливі взагалі |
| Windows-драйвери | Так: user-mode + USB `.sys` | Після v0.1 (v0.3) |
| Перша версія v0.1 | Усе: екран у Панелі керування, мережа, звук, GPU + DXVK | Довше до першого ISO, зате одразу видно всі класи багів |
| Збірка | GitHub Actions (Pro зі студентського пакета) + одноразові інстанси Google Cloud | На цій машині нічого не компілюємо |

---

## 3. Архітектура

### 3.1 Шари

```
┌────────────────────────────────────────────────────────────────────┐
│ ПРОГРАМИ           notepad.exe · game.exe · steam.exe · setup.exe  │
├────────────────────────────────────────────────────────────────────┤
│ ОБОЛОНКА            explorer.exe (ReactOS) · Панель керування      │
│                     taskmgr · regedit · devmgr · kbswitch · sndvol │
├────────────────────────────────────────────────────────────────────┤
│ СИСТЕМНІ ПРОЦЕСИ    wininit · csrss · dwm · services · svchost ·   │
│ (усі PE)            lsass · winlogon · audiodg · winedevice        │
├────────────────────────────────────────────────────────────────────┤
│ NT-РАНТАЙМ          ntdll · kernel32 · user32 · win32u · gdi32 ·   │
│ (Wine 11.x)         advapi32 · ole32 · rpcrt4 · ws2_32 · mmdevapi  │
│                     DXVK · vkd3d-proton · dxvk-nvapi · Mono · Gecko│
├────────────────────────────────────────────────────────────────────┤
│ МІСТ (наш код)      winearctic.drv · dwm/csrss unix-частини ·      │
│                     arcticpnp.sys · аудіодрайвер · Wlansvc · Dhcp  │
├────────────────────────────────────────────────────────────────────┤
│ ХОСТ (невидимий)    arctic-init · hostd · wineserver · udevd ·     │
│                     iwd · dbus · Mesa · NVIDIA userspace · alsa-lib│
├────────────────────────────────────────────────────────────────────┤
│ LINUX 7.x           DRM/KMS · ALSA · evdev · netdev · nl80211 ·    │
│                     USB · NVMe · HID · ntsync · NTFS               │
└────────────────────────────────────────────────────────────────────┘
```

### 3.2 Дерево процесів

```
Linux kernel
└─ arctic-init                  PID 1, root. Монтування, запуск бекендів, перезапуск NT-світу
   ├─ udevd                     модулі та прошивки під наявне залізо, гаряче підключення
   ├─ hostd                     привілейований брокер (розділ 3.10)
   ├─ iwd, dbus-daemon, (bluetoothd)
   └─ wineserver -p             uid nt. «Виконавча система NT»: об'єкти, хендли, реєстр, вікна, черги
      └─ wininit.exe
         ├─ csrss.exe           Raw Input Thread: libinput → wineserver; кнопка живлення, кришка
         ├─ dwm.exe             DRM/KMS, композиція, курсор, режими екрана
         ├─ services.exe
         │  ├─ svchost.exe      Audiosrv, AudioEndpointBuilder, Dhcp, Dnscache, Wlansvc,
         │  │                   netprofm, PlugPlay, Themes, W32Time, Spooler …
         │  ├─ audiodg.exe      мікшер
         │  └─ winedevice.exe   arcticpnp.sys, mountmgr.sys, winebus.sys, wineusb.sys
         ├─ lsass.exe
         └─ winlogon.exe → userinit.exe → explorer.exe → програми
```

Перезапуски як у Windows: впав `explorer.exe` — його підніме winlogon; впав `dwm.exe` —
wininit підніме його знову, а клієнти перепідключаться (у v0.1 допустимо перезапуск сеансу).
Впав `wineserver` — `arctic-init` перезапускає весь NT-світ і пише журнал.

### 3.3 Завантаження

```
UEFI / BIOS
└─ Limine                     BIOS+UEFI, ISO-hybrid. Меню: «Arctic», «Arctic — зберігати зміни»
   └─ Linux + initrd (~1–2 МБ, без модулів: USB/NVMe/AHCI, NTFS, squashfs, device-mapper, zram вбудовані в ядро)
      └─ /init (наш, статичний)
         1. ARCTIC.png на simpledrm
         2. знайти носій за міткою → host.sqfs
         3. C: — живий режим: знімок dm-snapshot над windows.img (NTFS) зі змінами в zram;
                 режим збереження: розділ ARCTIC-DATA (NTFS) напряму, з fsck.ntfs, якщо том «брудний»
         4. mount -t ntfs -o nocase,windows_names → C:, switch_root у host.sqfs
      └─ arctic-init (PID 1)
         udevd + coldplug → hostd, iwd, dbus → wineserver -p → wininit.exe
         dwm.exe забирає екран у splash без чорного кадру (той самий режим KMS)
```

Фатальна помилка до появи робочого столу (носій не змонтувався, dwm не стартував тричі поспіль):
init малює на DRM екран з кодом помилки і пише журнал на флешку.

### 3.4 Дисплей: dwm.exe

`dwm.exe` — PE-процес Wine з unix-частиною (`dwm.so`), яка:

- знаходить GPU через udev, відкриває DRM-вузол, робить атомарний modesetting;
- тримає внутрішній сервер буферів на сокеті `/run/arctic/dwm` (доступ лише uid nt);
- композитить через Vulkan або, якщо GPU немає, через pixman на CPU;
- керує апаратним курсором, кількома моніторами, гарячим підключенням, VRR.

**Потоки кадрів:**

```
GDI-вікно (Блокнот)                    D3D-гра
  win32u малює в DIB                     DXVK / vkd3d-proton → Vulkan
  (window_surface у memfd)               swapchain на VkSurface від winearctic.drv
        │ flush: damage + commit                │ dmabuf + drm_syncobj
        ▼                                       ▼
 ┌──────────────────────────── dwm.exe ─────────────────────────────┐
 │  геометрія і z-order ← спільна пам'ять wineserver (не від клієнта)│
 │  GPU:    Vulkan-композиція, тіні, альфа layered-вікон              │
 │          повноекранний кадр без перекриттів → прямо в KMS-план     │
 │          (0 копій, як Independent Flip у Windows)                  │
 │  CPU:    pixman → dumb-буфер, лише пошкоджені прямокутники        │
 └───────────────────────────────┬──────────────────────────────────┘
                                 ▼
                          DRM/KMS (Linux)
```

**Хто знає, де вікно.** Процеси не кажуть dwm, де стоїть їхнє вікно. `dwm.exe` сам є Wine-процесом
і читає дерево вікон, z-order та видимі регіони з `wineserver`, як справжній DWM читає їх з win32k.
Процес лише прив'язує буфер до HWND і комітить вміст із серійним номером конфігурації. Так розмір
буфера й прямокутник вікна збігаються при зміні розміру, без мерехтіння.

**Внутрішнє з'єднання** — підмножина Wayland-wire на нашому сервері:

| Протокол | Навіщо |
|---|---|
| `wl_compositor`, `wl_surface`, `wl_subsurface`, `wl_shm` | поверхні, GDI-буфери у спільній пам'яті |
| `zwp_linux_dmabuf_v1` (з feedback) | GPU-буфери; dwm підказує формати/модифікатори для прямого сканування |
| `wp_linux_drm_syncobj_v1` | explicit sync; критично для NVIDIA |
| `wp_presentation` | реальний час показу → frame pacing DXVK, `DwmGetCompositionTimingInfo` |
| `wp_fifo_v1`, `wp_commit_timing_v1` | VSync (FIFO) |
| `wp_tearing_control_v1` | VSync off у іграх |
| `wp_viewporter` | масштабування низьких роздільностей у повноекранному режимі |
| `arctic_surface_v1` (наш) | поверхня ↔ HWND, серійні номери конфігурації |
| `arctic_display_v1` (наш) | GPU, виходи, EDID, режими, зміна режиму, курсор |

**Чого немає:** `xdg-shell`, `wl_seat`, data-device, будь-якого вводу. Буфер-транспорт і нічого більше.

**Режими:**

- **GPU** — є render-вузол і Vulkan. Розподіл площин — libliftoff.
- **Базовий** — немає render-вузла (QEMU, `nomodeset`, невідомий GPU) або Vulkan не ініціалізувався.
  Композиція на CPU, як у Windows XP або з «Базовим адаптером дисплея Microsoft». 3D-програми
  отримують lavapipe (програмний Vulkan) — аналог WARP: DXVK працює, повільно.
- dwm стартує на тому, що є (simpledrm), і в пізніших версіях переїжджає на справжній GPU,
  коли udev приносить його DRM-пристрій, — як Windows підхоплює драйвер відеокарти без перезавантаження.
  У v0.1 достатньо дочекатися coldplug.

**Керування екраном (демонстрація концепції):**

```
desk.cpl (ReactOS)
  ChangeDisplaySettingsEx(1920x1080@144)
    → win32u → winearctic.drv: pChangeDisplaySettings
      → dwm.exe: atomic TEST_ONLY → commit
        → KMS перемикає режим → WM_DISPLAYCHANGE усім вікнам

EnumDisplayDevices / EnumDisplaySettingsEx
    ← win32u ← winearctic.drv: pUpdateDisplayDevices (add_gpu / add_source / add_monitor / add_modes)
      ← dwm.exe: GPU (PCI ID), конектори, EDID (libdisplay-info), режими, HDR-можливості
```

Інтерфейс драйвера Wine (`user_driver_funcs`, версія 110) уже має ці точки входу разом з EDID і
`hdr_enabled`. Нову модель вигадувати не треба — лише реалізувати її поверх KMS.

### 3.5 Ввід: csrss.exe

- **libinput** (path backend, пристрої від udev) у потоці Raw Input Thread.
- **Клавіатура:** скан-коди → `__wine_send_input` → `wineserver` → черга потоку з фокусом.
  Це апаратний ввід, без прапорця INJECTED — важливо для ігор. Скан-код → VK через Windows-розкладки
  (`kbdus`, `kbdur`, `kbdru` — з ReactOS, якщо у Wine немає). Alt+Shift перемикає розкладку
  у Windows-світі, індикатор у треї — `kbswitch` з ReactOS.
- **Миша:** відносні дельти → крива «Підвищена точність вказівника» як у Windows, параметри з `main.cpl`.
  Ігри отримують сирі дельти через `WM_INPUT`. `ClipCursor` і захоплення живуть у `wineserver`.
- **Курсор** рухається апаратною площиною dwm одразу від RIT, не чекаючи процесів.
- **Кнопка живлення, кришка** → політика живлення у winlogon. **Ctrl+Alt+Del** → SAS → winlogon.
- **Геймпади й HID** — окремо, як у Windows: `winebus.sys` (hidraw/evdev) → HID → XInput,
  DirectInput, Windows.Gaming.Input.

### 3.6 Графічні API

| API | Реалізація | Лежить у |
|---|---|---|
| GDI / USER | win32u → DIB → dwm | NT-рантайм |
| D3D 8/9/10/11 | DXVK → Vulkan | `C:\Windows\System32` (+ SysWOW64) як системні DLL |
| D3D12 | vkd3d-proton → Vulkan | те саме |
| DXGI / NVAPI | DXVK, dxvk-nvapi (DLSS на NVIDIA) | те саме |
| OpenGL | opengl32 → EGL на внутрішньому з'єднанні (Mesa / NVIDIA egl-wayland) | згодом — Zink як PE (OpenGL поверх Vulkan, EGL не потрібен) |
| Vulkan | winevulkan → драйвер GPU | Mesa: radv, anv, nvk, lavapipe; NVIDIA |
| Відео | VA-API (Wine 11.16), Vulkan Video | — |

Користувач не бачить слів «DXVK» чи «Proton»: це просто `d3d11.dll` у System32.

**Вибір драйвера NVIDIA:** udev-правило для `10de:03xx` пробує `nvidia` (open modules); якщо модуль
не прив'язався (карта до Turing), вантажиться `nouveau`. Vulkan-завантажувач сам бере ICD, що працює.
Модулі NVIDIA збираються в CI під конкретне ядро; ця пара версій закріплюється й оновлюється разом.

### 3.7 Звук

```
програма ── WASAPI / DirectSound / XAudio2 / waveOut
              └─ mmdevapi → наш драйвер (PE + unix)
                   └─ кільцевий буфер у спільній пам'яті на кожен потік
                        └─ audiodg.exe: мікшування float32, ресемплінг, гучність сеансу
                             └─ ALSA hw:X (напряму, без dmix)
```

- Мікшер гучності по програмах справжній: гучність сеансу застосовує audiodg.
- Exclusive mode: процес відкриває hw напряму, audiodg звільняє пристрій.
- AudioEndpointBuilder будує IMMDevice з ALSA + UCM-профілів (`alsa-ucm-conf` — без нього
  не працює звук на ноутбуках із SOF). HDMI-аудіо і USB-гарнітури — з гарячим підключенням.
- `mmsys.cpl`, `sndvol32` у треї, пристрій за замовчуванням — поверх цього.
- Bluetooth-аудіо (v0.3): BlueZ → A2DP-транспорт → audiodg; кодування SBC/AAC у Windows-світі.

### 3.8 Мережа

- **Сокети:** `ws2_32` → Linux-сокети напряму. TCP/IP-стек ядра відіграє роль tcpip.sys.
- **Адреси:** служба `Dhcp` (PE, свій DHCP-клієнт) для Ethernet і Wi-Fi. Адреси й маршрути
  через netlink застосовує hostd. DNS: `Dnscache`, а hostd пише `resolv.conf`.
- **Wi-Fi:** `wlanapi.dll` + служба `Wlansvc` → iwd (D-Bus) → nl80211. Профілі збережених мереж
  зберігаються як WLAN-профілі в реєстрі Windows; iwd лише асоціюється і робить WPA-рукостискання.
- **Інтерфейс:** `ncpa.cpl` (netshell з ReactOS), іконка мережі в треї зі списком Wi-Fi (пишемо свою).
- **NLA** (`netprofm`) повідомляє програмам, чи є інтернет.

### 3.9 Пристрої, диски, Windows-драйвери

- **`arcticpnp.sys`** (новий, у winedevice.exe). Unix-частина обходить sysfs, слухає udev і створює
  в дереві пристроїв Windows вузли з апаратними ID: `PCI\VEN_1002&DEV_67DF&SUBSYS_…&REV_…`,
  `USB\VID_…&PID_…`, `HDAUDIO\…`, `ACPI\…`. Драйвер вузла — ім'я модуля Linux і його версія.
  Диспетчер пристроїв (devmgr з ReactOS) показує реальне залізо. «Вимкнути пристрій» →
  hostd відв'язує Linux-драйвер через sysfs `unbind`.
- **Диски:** `mountmgr.sys` отримує Linux-бекенд на udev (у Wine він ходить в UDisks2 через D-Bus).
  hostd монтує ntfs, exfat, vfat, ext4, btrfs → літери D:, E: з гарячим підключенням.
- **Windows-драйвери (v0.3):**
  - USB `.sys`: `ntoskrnl.exe` + `wineusb.sys` вантажать драйвер у користувацькому просторі поверх libusb.
    Коли для VID/PID встановлено INF, hostd відв'язує від пристрою Linux-драйвер.
  - User-mode: драйвери принтерів (DLL у спулері), TWAIN-джерела, UMDF.
  - Ядрові `.sys` для PCI неможливі в принципі.

### 3.10 Привілеї: hostd

Увесь Windows-світ працює від одного непривілейованого Linux-uid `nt`. Усередині — NT-токен
адміністратора, UAC вимкнено, входу немає. Доступ до пристроїв дають udev-правила й групи:
`/dev/dri`, `/dev/input`, `/dev/snd`, `/dev/ntsync`, `/dev/hidraw*`.

Те, що потребує root, виконує `hostd` — маленький брокер з фіксованим набором запитів:

- mount / umount, створення розділу збереження;
- адреси, маршрути, DNS;
- reboot, poweroff, suspend;
- системний час і RTC;
- sysfs bind/unbind драйверів.

Це єдині «двері» з Windows-світу в Linux. Навіщо так: уразливість у браузері не дає автоматично
root над ядром.

### 3.11 Файлова система і LiveUSB

```
USB (ISO-hybrid, BIOS + UEFI)
├─ boot/ (Limine), EFI/BOOT/BOOTX64.EFI
├─ arctic/vmlinuz, arctic/initrd.img
├─ arctic/host.sqfs     Linux-хост + PE/unix-модулі Wine + Mono/Gecko (zstd)
├─ arctic/windows.img   NTFS: C:\Windows, Program Files, Users\Default, реєстр
└─ ARCTIC-DATA          розділ NTFS, створюється при першому «зберігати зміни»
```

- **C: завжди справжній NTFS** — і в живому режимі, і в режимі збереження. Overlayfs не використовується:
  він змішав би семантику двох ФС.
  - **Живий режим:** device-mapper snapshot над `windows.img`, зміни пишуться в zram (стиснена RAM)
    і зникають після перезавантаження.
  - **Режим збереження:** під час першого запуску `windows.img` розгортається в розділ ARCTIC-DATA
    на вільному місці флешки і розширюється на весь розділ. Далі цей розділ монтується напряму як C:.
    На будь-якому Windows-ПК він відкривається як звичайний диск.
- **Опції монтування:** `nocase=1` — ядро шукає імена без урахування регістру, як Windows;
  `windows_names=1` — не дає створити CON, NUL та інші заборонені Windows імена.
  Wine поки не знає про `nocase` на NTFS (він перевіряє лише `FS_CASEFOLD_FL` ext4) і при промаху
  все одно сканує каталог. Невеликий патч у нашому форку прибере це сканування.
- **Реєстр** зберігається в `C:\Windows\System32\config\` (невеликий патч `wineserver`: шлях до хайвів),
  як і в справжній Windows. Службовий каталог Wine (`dosdevices`, сокет wineserver) лежить у tmpfs хоста
  і створюється під час завантаження. **Диск Z: (корінь Linux) не створюється.**
- **DOS-атрибути й дескриптори безпеки** Wine зараз тримає в xattr. Пізніше — відображати їх на справжні
  атрибути NTFS і `$Secure`, щоб «прихований» файл був прихованим і на іншому Windows-ПК.
- Великі PE-модулі Wine живуть у стиснутому `host.sqfs`. DXVK і vkd3d-proton лежать справжніми
  файлами в System32. Так `windows.img` лишається малим (≈1–1.5 ГБ), навіть без стиснення.
- Після v0.1 (встановлення на диск): C: — розділ NTFS, `host.sqfs` і ядро — файлами
  в `C:\Windows\System32\Host\` та ESP. Linux-хост тоді буквально лежить усередині C:\Windows.

### 3.12 Ідентичність, мова, вигляд

- **Версія:** Windows 11 25H2, 10.0.26200. Це видно в `RtlGetVersion`, у реєстрі
  (`CurrentBuild`, `DisplayVersion`, `ProductName`) і в WMI `Win32_OperatingSystem`.
- **Сумісність:** вкладка «Сумісність» у властивостях EXE → `AppCompatFlags\Layers` → профіль версії
  Wine для цієї програми.
- **WMI:** реальні дані заліза: DMI (виробник, модель), CPU, GPU (від dwm), диски.
- **Брендинг** у `winver` і `sysdm.cpl` — Arctic.
- **Мова:** uk-UA — і системна, і інтерфейсу. Розкладки EN/UA/RU, Alt+Shift.
- **Оболонка:** ReactOS explorer у класичному макеті (панель знизу, класичний Пуск), visual style Mizu
  через uxtheme. Запасний варіант — explorer Wine, якщо ReactOS explorer не запрацює (розділ 10).
- **Шрифти:** вільні метрично сумісні (Liberation замість Arial/Times/Courier, Tahoma з Wine)
  + `FontSubstitutes` для Segoe UI. Кирилиця обов'язкова.

### 3.13 Журнали

- Консолі немає. Журнали лежать у `C:\Windows\Logs\Arctic\` і відкриваються Блокнотом:
  `kernel.log` (з `/dev/kmsg`), `host.log`, `dwm.log`, `csrss.log`, журнали Wine з каналами
  `WINEDEBUG`, заданими в реєстрі. У режимі збереження вони переживають перезавантаження.
- **dev-збірка** — прапорець CI, не пункт меню: serial-консоль із shell для QEMU і детальні журнали.

---

## 4. Міст: Windows API → Linux

| Windows API / компонент | Наша реалізація | Linux під капотом |
|---|---|---|
| ChangeDisplaySettingsEx, EnumDisplayDevices, EnumDisplaySettings | win32u → winearctic.drv → dwm.exe | DRM atomic KMS, EDID |
| GDI, USER, вікна | win32u + wineserver → dwm.exe | KMS dumb-буфери / Vulkan |
| D3D9–11 / D3D12 / DXGI | DXVK / vkd3d-proton | Vulkan: radv, anv, nvk, NVIDIA |
| OpenGL | opengl32 → EGL | Mesa / NVIDIA |
| SendInput, Raw Input, повідомлення миші/клавіатури | csrss.exe (RIT) → wineserver | libinput, evdev |
| XInput, DirectInput, HID | winebus.sys | hidraw, evdev |
| WASAPI, DirectSound, XAudio2 | mmdevapi → audiodg.exe | ALSA hw |
| Winsock | ws2_32 | сокети ядра |
| IP Helper, DHCP, DNS | iphlpapi, служби Dhcp / Dnscache | netlink (через hostd) |
| WLAN API | wlanapi → Wlansvc | iwd → nl80211 |
| SetupAPI, CfgMgr32, Диспетчер пристроїв | arcticpnp.sys, PlugPlay | sysfs, udev, bind/unbind |
| Volume / Mount Manager | mountmgr.sys | udev + hostd mount |
| Синхронізація NT (події, м'ютекси, семафори) | ntdll | `/dev/ntsync` |
| ExitWindowsEx, SetSuspendState, GetSystemPowerStatus | winlogon, powrprof | hostd, `/sys/power`, `/sys/class/power_supply` |
| SetSystemTime, часові пояси | kernel32 → hostd | settimeofday, RTC, tzdata |
| Bluetooth API (v0.3) | bthserv, bthprops | BlueZ |
| WinUSB, USB `.sys` (v0.3) | wineusb.sys + ntoskrnl.exe | libusb / usbfs |
| Друк (v0.3) | спулер + IPP port monitor | мережа (IPP Everywhere) |

---

## 5. Що лишається від Linux

Хост збирається з бінарних пакетів Arch (glibc того ж покоління), але береться лише замикання
залежностей наших програм плюс allowlist даних. Це не Arch, а набір файлів.

- **Ядро 7.x:** модулі (усі драйвери — модулями), прошивки (amdgpu, radeon, i915/xe, NVIDIA GSP,
  iwlwifi, rtw88/89, mt76, ath, brcm, Bluetooth, SOF, Realtek NIC).
- **База:** glibc, libgcc/libstdc++, kmod (через udevd), ntfsprogs-plus (`mkfs.ntfs`, `fsck.ntfs`;
  створення й перевірка ARCTIC-DATA), tzdata, ca-certificates.
- **Пристрої:** udevd + hwdb + правила, libudev, libinput, libevdev, mtdev.
- **Графіка:** libdrm, Mesa (radv, anv, nvk, lavapipe; radeonsi, iris, llvmpipe; GBM, EGL),
  vulkan-icd-loader, libglvnd, NVIDIA userspace + egl-wayland, libwayland (лише внутрішній транспорт),
  pixman, libliftoff, libdisplay-info.
- **Звук:** alsa-lib, alsa-ucm-conf.
- **Мережа й Bluetooth:** iwd, dbus-daemon, bluez (v0.3).
- **Для unix-частини Wine:** freetype, gnutls (+ nettle, gmp, libtasn1, p11-kit), zlib, zstd, xz,
  libusb, SDL (геймпади), gstreamer + плагіни (медіа), libva.
- **Немає:** systemd як init, shell (у релізі), coreutils, X11, Wayland-композитора, PipeWire,
  PulseAudio, NetworkManager, logind, Python, пакетного менеджера.

---

## 6. Що пишемо самі

Усе, що стосується NT, живе у форку Wine в **нових модулях**, щоб ребейз на upstream лишався дешевим.

| Модуль | Де | Мова |
|---|---|---|
| `/init` (initrd), `arctic-init` (PID 1), `hostd`, splash і екран помилки | `host/init/` | C, статично |
| `winearctic.drv` — дисплейний драйвер (форк winewayland.drv без xdg-shell, seat, clipboard) | `wine/dlls/winearctic.drv` | C (PE + unix) |
| `dwm.exe` + `dwm.so` — KMS, сервер буферів, Vulkan/pixman-композиція | `wine/programs/dwm` | C |
| `csrss.exe` — RIT, libinput, кнопки живлення | `wine/programs/csrss` | C |
| `wininit.exe`, `winlogon.exe`, `userinit.exe` — старт сеансу, SAS, завершення роботи | `wine/programs/*` | C |
| Аудіо: драйвер mmdevapi, `Audiosrv`, `AudioEndpointBuilder`, `audiodg.exe` | `wine/dlls/…`, `wine/programs/audiodg` | C |
| Мережа: `Dhcp`, `Dnscache`, `Wlansvc` + `wlanapi.dll` (iwd), `netprofm`, Wi-Fi-flyout у треї | `wine/dlls/…` | C |
| `arcticpnp.sys`; Linux-бекенд для `mountmgr.sys` | `wine/dlls/…` | C |
| Реєстр за замовчуванням: Windows 11, uk-UA, Mizu, автологін, служби, без Z: | `windows/registry/` | .reg |
| Збірка образу, CI, одноразовий збирач GCE | `image/`, `ci/` | shell |

Зміни в `win32u`, `wineserver`, `ntdll` — мінімальні: хук для dwm на дерево вікон, машинний
`wineserver` без таймауту, прибраний Z:, хайви реєстру в `C:\Windows\System32\config\`,
розпізнавання `nocase` на NTFS. Кожна — окремим комітом.

---

## 7. Репозиторій і збірка

### 7.1 Структура

```
arctic/                          приватний GitHub-репозиторій (Pro зі студентського пакета)
├─ docs/PLAN.md
├─ kernel/        arctic.config (фрагмент поверх конфігу Arch), версії ядра і NVIDIA
├─ host/
│  ├─ init/       /init, arctic-init, hostd, splash
│  └─ rootfs/     packages.txt, allowlist, udev-правила, конфіги iwd/dbus
├─ runtime/
│  ├─ wine/       submodule → форк upstream Wine, гілка arctic
│  └─ manifest.toml   DXVK, vkd3d-proton, dxvk-nvapi, Wine Mono, Wine Gecko: версії + sha256
├─ reactos/       що беремо з нічних збірок ReactOS (x86 і x64); пізніше — власний форк
├─ windows/       registry/, fonts/
├─ image/         host.sqfs, windows.img, initrd, ISO (Limine + xorriso), ARCTIC.png
├─ ci/            workflows GitHub Actions, gce/
└─ tools/         завантажити ISO, запустити QEMU, записати флешку
```

### 7.2 Конвеєр

```
push → GitHub Actions, контейнер archlinux:latest
  1. kernel   кеш за хешем (версія + конфіг) → vmlinuz, модулі; nvidia-open під це ядро
  2. wine     ccache; --enable-archs=i386,x86_64 (новий WoW64); mingw → PE, gcc → unix .so
  3. rootfs   pacstrap у staging → відбір за ELF-залежностями + allowlist → host.sqfs
  4. prefix   wineboot у контейнері без дисплея → реєстр, C:\Windows → windows.img (mkfs.ntfs)
  5. image    initrd + Limine + xorriso → arctic-<commit>.iso
  6. smoke    QEMU: завантаження до робочого столу, маркери в serial-журналі, знімок екрана
  → артефакт: ISO + знімок → `gh run download` у out/ на цій машині
```

- **GitHub Actions:** 3000 хв/міс у приватному репозиторії (Pro). Кешуються ядро (за хешем конфігу)
  і ccache Wine, тож повна збірка буває рідко.
- **Google Cloud:** `ci/gce/build.sh` піднімає spot-VM з `--max-run-duration` і
  `--instance-termination-action=DELETE`. Вона збирає, кладе ISO в GitHub Release або Cloud Storage
  і видаляється сама, навіть якщо щось зависло. Використовується для повних збірок без кешу,
  або якщо раннеру GitHub не вистачить диска (~14 ГБ вільних за замовчуванням) чи 6-годинного ліміту.
  Бюджету ~$100 вистачить на десятки збірок.
- **Ця машина:** лише ISO (≈2.5–3.5 ГБ, в `out/` тримаємо тільки останній), QEMU у `tools/`
  для базового режиму, флешка для заліза. Нічого важкого, нічого на C:.

### 7.3 Тестування

- **QEMU TCG тут:** базовий режим (bochs / virtio-gpu 2D, композиція на CPU, lavapipe для Vulkan).
  Журнали через serial.
- **Залізо:** RX 580 (radv), RTX 5070 (nvidia-open; nouveau+NVK як запасний), RTX 3060, Intel iGPU на ноутбуці.
- **CI:** кожен push завантажує образ у QEMU і перевіряє, що dwm показав перший кадр,
  а explorer створив панель задач.

---

## 8. Етапи до v0.1

Порядок обрано так, щоб найбільші технічні ризики знялися першими. Терміни — порядок величини, не обіцянка.

### M0 — Інфраструктура (≈1–2 тижні)
- Репозиторій, кроки конвеєра 1–6 з upstream Wine без змін.
- Ланцюжок: ядро → arctic-init → wineserver → wineboot → services.exe, у serial-журналі маркер готовності.
- ARCTIC.png на екрані від початку завантаження.
- **Приймання:** ISO з CI, скачаний сюди, стартує в QEMU і на RX 580. Розмір ISO і час завантаження заміряно.

### M1 — Перші пікселі (≈4–8 тижнів)
- `winearctic.drv`, `dwm.exe` у базовому режимі, `csrss.exe`, `wininit` / `winlogon` / `userinit`.
- Перевірка ReactOS explorer на старті етапу (розділ 10); Блокнот, Диспетчер задач, regedit.
- uk-UA, розкладки EN/UA/RU, Alt+Shift, Mizu.
- Ранній тест: Vulkan-вікно через lavapipe, щоб перевірити шлях WSI до етапу GPU.
- **Приймання:** у QEMU — робочий стіл без X і Wayland. Вікна рухаються, перекриваються, фокус
  правильний, український текст у Блокноті. У Диспетчері задач видно csrss.exe, dwm.exe, explorer.exe.

### M2 — Екран у Панелі керування (≈2–3 тижні)
- `pUpdateDisplayDevices` / `pChangeDisplaySettings` ↔ KMS, EDID, режими, кілька моніторів, гаряче підключення.
- `desk.cpl` з ReactOS.
- **Приймання:** на залізі зміна роздільності й частоти в desk.cpl реально перемикає монітор;
  другий монітор підхоплюється на льоту з правильною назвою моделі.

### M3 — GPU (≈4–8 тижнів)
- Vulkan-композиція, dmabuf, syncobj, presentation / fifo / tearing, пряме сканування, libliftoff.
- Автовибір драйвера NVIDIA; Mesa для AMD та Intel.
- DXVK, vkd3d-proton, dxvk-nvapi у System32. Автоматичний базовий режим, коли GPU недоступний.
- **Приймання:** на RX 580, RTX 5070, RTX 3060 та Intel повноекранна D3D11-гра йде через пряме
  сканування, FPS у межах ~5% від тієї ж гри в Proton на звичайному Linux на тій самій машині.
  У QEMU все працює в базовому режимі.

### M4 — Звук (≈3–5 тижнів)
- Драйвер mmdevapi, Audiosrv, AudioEndpointBuilder, audiodg.exe на ALSA hw, UCM.
- mmsys.cpl, sndvol32 у треї, мікшер по програмах.
- **Приймання:** звук з гри і браузера одночасно; перемикання HDMI ↔ аналог у mmsys.cpl;
  USB-гарнітура підхоплюється на льоту.

### M5 — Мережа (≈3–5 тижнів)
- Dhcp, Dnscache, Wlansvc + wlanapi на iwd, netprofm, ncpa.cpl, Wi-Fi-список у треї.
- **Приймання:** Ethernet піднімається сам; Wi-Fi WPA2/WPA3 підключається з трею і пам'ятається
  в режимі збереження; `ipconfig` і `ping` показують правду.

### M6 — v0.1 (≈2–4 тижні)
- arcticpnp.sys і Диспетчер пристроїв (перегляд + вимкнення), автомонтування флешок і дисків як D:/E:.
- Режим збереження (ARCTIC-DATA), завершення роботи й перезавантаження з Пуску, журнали.
- Прогін на всіх чотирьох машинах.
- **Приймання:** повний сценарій на кожній машині — завантажитись, підключити Wi-Fi, змінити
  роздільність, увімкнути звук, пограти в D3D11-гру, вимкнути ПК з Пуску.

**Разом до v0.1: ≈5–9 місяців.**

---

## 9. Після v0.1

- **v0.2 — ігри й браузер.** Windows-клієнт Steam як звичайна програма: ігри говорять з ним напряму,
  тож мости Proton `lsteamclient` і `steam.exe` не потрібні. Міст EAC/BattlEye з Proton. Chrome,
  а в dwm — DirectComposition (`dcomp.dll`), якої Wine не має: композитор у нас свій. Профілі
  сумісності у властивостях EXE.
- **v0.3 — пристрої.** Bluetooth через BlueZ (пари, HID, геймпади, A2DP в audiodg). USB `.sys`
  і user-mode драйвери Windows. Друк IPP.
- **v0.4 — встановлення на диск.** Інсталятор як Windows-програма, C: на розділі NTFS,
  A/B-оновлення хоста з відкатом.
- **v0.5 — можливості dwm і живлення.** Мініатюри Alt+Tab і панелі задач, DWM API, HDR, VRR, DPI;
  сон, батарея, яскравість; жести тачпада. Переїзд dwm з simpledrm на GPU без перезавантаження.
- **Далі.** Zink як PE, ARM64 (FEX + ARM64EC), власна оболонка.

---

## 10. Ризики і ранні перевірки

| Ризик | Як знімаємо |
|---|---|
| Vulkan WSI драйверів GPU (особливо NVIDIA + explicit sync) на нашому мінімальному сервері буферів | Тест на lavapipe вже в M1; на початку M3 першою — NVIDIA |
| win32u без зовнішнього віконного менеджера | Це режим «віртуального робочого столу» Wine, він зрілий. Ризик у деталях (ресайз, layered-вікна) — закриваємо серійними номерами конфігурації |
| ReactOS explorer у Wine вже раз зависав у browseui | Перевірка на старті M1; запасний варіант — explorer Wine з панеллю задач |
| Модулі NVIDIA під нове ядро | Версії ядра і nvidia-open закріплені парою, оновлюються разом |
| NTFS-драйвер без журналювання: після раптового вимкнення живлення C: може пошкодитися | Живий режим не страждає: зміни в RAM, `windows.img` лише для читання. У режимі збереження: `fsck.ntfs` на кожному старті, якщо том «брудний»; кнопка живлення завжди веде до коректного завершення з розмонтуванням; флешку можна перевірити `chkdsk` на будь-якому Windows-ПК. Коли в драйвері з'явиться журналювання — просто оновлюємо ядро |
| Новий драйвер NTFS вийшов у червні 2026 | Запасний варіант у тому ж ядрі — `ntfs3`; вибір драйвера — одна опція в init |
| Злиття з upstream Wine | Код у нових модулях, правки ядра рантайму окремими комітами, ребейз на кожен dev-реліз або щомісяця |
| Ліміти CI (диск, 6 год) | Кеші + GCE |
| Ввід від мишей 1000+ Гц через wineserver | Заміряти в M3 |
| Обсяг роботи | Кожен етап закінчується ISO; порядок M1 → M3 знімає найбільші ризики першими |

---

## 11. Ліцензії

- Wine — LGPL-2.1+. ReactOS — переважно GPL-2.0+ / LGPL. DXVK — zlib. vkd3d-proton — LGPL-2.1.
  Mesa — MIT. Linux — GPL-2.0.
- NVIDIA userspace — ліцензія NVIDIA (дистрибутиви її розповсюджують; умови перечитати перед
  публічним ISO). Прошивки — кожна зі своєю ліцензією.
- Поки репозиторій приватний і ISO ніде не публікується, питань немає. Публічний ISO означає обов'язок
  опублікувати сирці: наші зміни Wine, ReactOS і конфіг ядра.
- Наш код — LGPL-2.1+, як у Wine, щоб його можна було вільно переносити між форком і власними модулями.

---

## 12. Наступний крок

**M0:** створити приватний репозиторій, написати кроки конвеєра 1–6 і отримати перший ISO,
який доходить до `services.exe` в QEMU.

Для цього потрібно:
1. Назва репозиторію і перевірка `gh auth status`.
2. Пізніше, для GCE: ID проєкту Google Cloud і сервісний акаунт з правом створювати VM.

---

## Джерела (стан на 2026-09-19)

- Wine 11.0: NTSYNC, завершений WoW64 — https://www.phoronix.com/news/Wine-11.0-January-2026 ,
  https://9to5linux.com/wine-11-officially-released-with-ntsync-support-vulkan-h-264-decoding-and-more
- Wine 11.15 / 11.16 (VA-API) — https://www.winehq.org/news/2026080801 ,
  https://alternativeto.net/news/2026/8/wine-11-16-brings-va-api-hardware-video-decoding-improved-arm64-support-and-35-bug-fixes/
- Wine 11.5, Syscall User Dispatch — https://www.phoronix.com/news/Wine-11.5-Released
- Zink як PE у Wine (пропозиція) — https://www.gamingonlinux.com/2026/04/a-future-wine-release-could-use-zink-to-run-opengl-via-vulkan/
- Інтерфейс дисплейного драйвера Wine — https://github.com/wine-mirror/wine/blob/master/include/wine/gdi_driver.h ,
  https://github.com/wine-mirror/wine/blob/master/include/wine/vulkan_driver.h
- Proton 11.0-1 — https://www.gamingonlinux.com/2026/07/proton-11-0-1-officially-released-to-expand-windows-games-on-steamos-linux/
- Linux 7.1 (новий NTFS) і 7.2 — https://www.linuxjournal.com/content/linux-kernel-71-officially-released-new-ntfs-driver-intel-fred-and-major-code-cleanup ,
  https://www.phoronix.com/news/Linux-7.2-Released
- Новий драйвер NTFS: опції монтування (`nocase`, `windows_names`) — https://docs.kernel.org/7.1/filesystems/ntfs.html ;
  можливості, ntfsprogs-plus, статус журналювання — https://lwn.net/Articles/1048627/ ,
  https://linuxiac.com/linux-kernel-7-1-merges-new-ntfs-driver-with-full-write-support/
- Wine і `EXT4_CASEFOLD_FL` (чому Wine не знає про `nocase`) — https://www.winehq.org/pipermail/wine-devel/2019-June/147467.html
- NVK і Blackwell — https://docs.mesa3d.org/drivers/nvk.html , https://www.phoronix.com/review/nvidia-nvk-linux-618-mesa-26
- ReactOS 0.4.16 — https://reactos.org/project-news/reactos-0416-released/ ; теми (Mizu) — https://github.com/reactos/reactos/tree/master/media/themes
- Longene (єдиний попередник) — https://en.wikipedia.org/wiki/Longene
- Ціни GitHub Actions 2026 — https://github.com/resources/insights/2026-pricing-changes-for-github-actions
