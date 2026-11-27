# MSI Afterburner і RTSS без змін

Мета — остання версія MSI Afterburner і RTSS, встановлена звичайним
інсталятором, робить усе, що на Windows: бачить карту, розганяє (частоти,
пам'ять, напруга, крива V/F, ліміти), керує вентиляторами, показує всі
датчики, оверлей і обмеження FPS у будь-якій грі. **На будь-якому ПК з
будь-якою картою NVIDIA чи AMD**: що вміє карта, визначає її драйвер на
старті, таблиць «модель → значення» немає. Програму не змінюємо — виправляємо
Arctic (stock-windows-programs). Наш «Тюнінг» і Afterburner не конфліктують
(розділ «Без конфліктів»).

## Перевірено 21.11.2026

Розібрано встановлені Afterburner 4.6.5 і RTSS 7.3.4 (`D:\Program Files (x86)`;
останні версії — та сама будова, трасу останньої зробити кроком 1) і драйвер
NVIDIA 580.178.04 з образу. Проби — на GTX 1050 Mobile (Pascal, закритий
модуль), лише читання і запис тих самих значень назад (стан не змінився).

**Що кличе Afterburner** (`RTHAL.dll` — увесь апаратний код):

- NVAPI (`nvapi.dll`, програма 32-бітна): 99 функцій, таблицею кодів
  `push id; call QueryInterface`. Серед них приватні: ClockBoostTable/Mask/
  Ranges/Lock, VFPCurve, Pstates20, VoltRails (Voltage %), PowerPolicies,
  ThermalPolicies, PerfPolicies (причини обмежень), FanCoolers, FanPolicies,
  FanArbiters, CoolerLevels, I2C, DRS.
- ADL (`atiadlxx.dll`/`atiadlxy.dll`): Overdrive5, Overdrive6, OverdriveN,
  Overdrive8, `ADL2_New_QueryPMLogData_Get`, `ADL_Display_WriteAndReadI2C`
  (контролери напруги сторонніх плат), AdapterInfo, VideoBiosInfo.
- D3DKMT: EnumAdapters2, OpenAdapterFromGdiDisplayName/DeviceName,
  QueryAdapterInfo, **QueryStatistics** (уніфіковане завантаження ГП, плагін
  GPU.dll). Також `NtQuerySystemInformation`, SetupDi (клас дисплея),
  `ExtEscape`, `nvldumd(x).dll` (версія драйвера).
- `RTCore64.sys` (через `RTCore.dll`, служба SCM): 17 IOCTL —
  `0x80002000/04` відображення `\Device\PhysicalMemory`, `…08–…28` порти
  in/out, `…30/34` rdmsr/wrmsr, `…40/44` відображення BAR (MmMapIoSpace),
  `…48/4C` читання/запис пам'яті, `…50/54` PCI config (HalGet/SetBusData).
- Плагіни: CPU.dll (MSR через RTCore), GPU.dll (D3DKMTQueryStatistics),
  PerfCounter (PDH), SMART (DeviceIoControl), Ping (IcmpSendEcho2),
  HwInfo/AIDA64 (спільна пам'ять тих програм). Автозапуск —
  `StartupViaTaskScheduler`; гарячі клавіші — DINPUT8; LogitechLcd.

**Що кличе RTSS** (`RTSSHooks64.dll`): `SetWindowsHookExA` +
`CreateRemoteThread` (впровадження), хуки D3D8/9/10/11/12, `D3D11On12CreateDevice`
(оверлей у D3D12), Vulkan (повний набір `vk*` + неявний шар `VK_LAYER_RTSS`,
`Vulkan\RTSSVkLayer64.json`), OpenGL (`wglSwapBuffers`),
**`D3DKMTGetScanLine`** (Scanline Sync), `NtSet/QueryTimerResolution`,
`DwmGetCompositionTimingInfo`, маркери Reflex через `nvapi64.dll`,
`D3DCOMPILER_47`. Запис відео — `EncoderServer64` + NVENC.dll, QSV.dll
(є програмний `libmfxsw64.dll`), VCE/AMF.

**NVIDIA під Linux має власну NVAPI** — `libnvidia-api.so.1` (з драйвера 525;
є в пакеті 580, ставиться в образ `install-nvidia-userspace.py`). 547
функцій, ходить у RM ядра напряму (ioctl), повз NVML. З 99 функцій
Afterburner у ній **63 — усі розгінні й датчики**; немає 36: дисплейні,
DRS, ідентифікатори, `GetMemoryInfo`, версія драйвера, I2C — їх дає
dxvk-nvapi або робимо самі. Майже всі версії структур, які передає
Afterburner (`size | ver<<16`), бібліотека знає дослівно.

Проба GET (усі функції, 32-бітні структури = 64-бітні, вказівників немає):

| Що | root | користувач |
|---|---|---|
| назва, BIOS, PCI, шина, P-state, частоти, Pstates20, DynamicPstates | OK | OK |
| крива V/F (ClockBoostMask/Table/Ranges, VFPCurve, ClockBoostLock) | OK | OK |
| напруга (VoltRails status), Voltage % (VoltRails control), GetVoltages | OK | OK |
| потужність (PowerPolicies, PowerTopology), температура (ThermalPolicies, ThermalSettings) | OK | OK |
| причини обмежень (PerfPolicies), ThermChannelGetInfo (hotspot/пам'ять) | OK | OK |
| FanCoolers*, FanArbiters, GetTachReading | NOT_SUPPORTED (ноутбук: вентилятори — EC) | — |

Проба SET (те саме значення назад):

| Що | root | користувач |
|---|---|---|
| крива V/F (`SetClockBoostTable`) | **OK** | INVALID_USER_PRIVILEGE |
| Voltage % (`SetCoreVoltageBoostPercent`) | **OK** | INVALID_USER_PRIVILEGE |
| ліміт потужності (`ClientPowerPoliciesSetStatus`) | **OK** | INVALID_USER_PRIVILEGE |
| фіксація точки (`SetClockBoostLock`, Ctrl+L у редакторі кривої) | **OK** | INVALID_USER_PRIVILEGE |
| зсув ядра (`SetPstates20`, P0, домен 0; діапазон −200…+1200 МГц) | **OK** | — |
| зсув пам'яті (`SetPstates20`, P0, домен 4; ±1000 МГц) | **OK** | — |
| ліміт температури (`SetThermalPoliciesStatus`) | NOT_SUPPORTED (мобільна карта) | — |

Висновок: **пропатчений модуль NVIDIA не потрібен** — той самий RM, що на
Windows, приймає ті самі команди; запис — лише від root (тобто через
arctic-init), що й треба. Відмови ноутбука (EC-вентилятори, ліміт
температури мобільної карти) — відмови драйвера, на Windows ті самі.

Інше на хості: модуль NVIDIA дає шину `NVIDIA i2c adapter 1 at 1:00.0`
(`CONFIG_I2C_CHARDEV=m`), amdgpu — `AMDGPU i2c hw/bit bus`.

**amdgpu у ядрі образу (7.2.6)**: `OD_SCLK/OD_MCLK` — smu7 (Tonga, Fiji,
Polaris), Vega10/12/20, Navi і новіші; `OD_VDDC_CURVE` — Vega20, Navi1x;
`OD_VDDGFX_OFFSET` — RDNA2, RDNA3, RDNA4; `gpu_od/fan_ctrl` (крива
вентилятора) — RDNA3, RDNA4; `gpu_metrics` — усі сучасні. **SI/CIK (GCN1/2,
HD 7000 / R9 200) Overdrive не мають зовсім.** Увімкнений Overdrive
(`amdgpu.ppfeaturemask` біт 0x4000) позначає ядро `TAINT_CPU_OUT_OF_SPEC`
(лише позначка).

**Wine 11.18 + патчі Arctic**: `NtGdiDdDDIQueryStatistics` — заглушка
(нулі); `D3DKMTGetScanLine` gdi32 не експортує; ETW
(`StartTrace/OpenTrace/ProcessTrace`) — заглушки; `NtSetTimerResolution` —
напівзаглушка; `atiadlxx` немає; `vulkan-1.dll` — вбудований Wine, неявних
шарів **не вантажить** (RTSS, а також уже зареєстровані тут OBS і Steam
Overlay у Vulkan-іграх не працюють), хоча ICD `winvulkan.json` зареєстровано.
`nvapi.dll` (32) і `nvapi64.dll` — dxvk-nvapi 0.9.2.

## Устрій

```
Afterburner / RTSS (Windows, без змін)
 ├─ nvapi.dll / nvapi64.dll ── dxvk-nvapi (D3D, Reflex, дисплеї, DRS)
 │                          └─ частина ГП ─┐
 ├─ atiadlxx.dll / atiadlxy.dll (новий) ───┼─► unix: gpu-oc (спільна модель карт за PCI)
 ├─ rtcore64.sys (вбудований) ─────────────┘     читання — у процесі (libnvidia-api, sysfs)
 │                                                запис — ArcticPowerRequest ─► arctic-init (root)
 └─ gdi32/win32u D3DKMT, vulkan-1, sechost ETW                 ├─ libnvidia-api.so.1 (SET)
                                                               ├─ amdgpu sysfs / hwmon
                                                               ├─ MSR, PCI config, SMN (читання)
                                                               └─ /dev/i2c-N (адреси VRM)
```

- **Модель карт** — одна для NVAPI, ADL, D3DKMT, RTCore: ключ — PCI-адреса.
  Хендли NVAPI (у 32-бітному процесі — свої 32-бітні), індекси адаптерів
  ADL, LUID — на неї. Optimus, гібриди, кілька карт — однаково.
- **NVIDIA**: для 63 функцій — виклик тієї ж функції `libnvidia-api` з тією ж
  структурою (версію, якої бібліотека не знає, перекладаємо). GET — у
  процесі від користувача; SET — запит до arctic-init з ID функції і
  структурою: білий список SET-функцій, межі — з відповідної GetInfo /
  Ranges, далі `libnvidia-api` від root. Решта: дисплейні, версія драйвера,
  пам'ять — dxvk-nvapi/NVML; DRS — справжнє сховище (реєстр) замість
  заглушок, відомі параметри (режим живлення, ліміт кадрів) — на їхні
  механізми Arctic; I2C — `/dev/i2c-N` адаптера NVIDIA через arctic-init,
  лише адреси контролерів напруги; `GetSystemType` — з DMI (ноутбук/ПК);
  `nvldumd(x).dll` — заглушка з ресурсом версії драйвера.
- **AMD**: новий `atiadlxx`/`atiadlxy` з тими функціями, що в RTHAL:
  Overdrive8 ↔ `pp_od_clk_voltage` (OD_SCLK/OD_MCLK/OD_VDDGFX_OFFSET),
  `power1_cap`, `gpu_od/fan_ctrl`; OverdriveN (Polaris/Vega) ↔ стани з мВ,
  `pwm1`; Overdrive5/6 ↔ найближче, що дає карта; PMLog ↔ `gpu_metrics` +
  hwmon; I2C ↔ `/dev/i2c-N` amdgpu. Біт Overdrive у `amdgpu.ppfeaturemask`
  — в образі для всіх (`/etc/modprobe.d/arctic-gpu.conf`). **GCN1/2** —
  свій патч ядра: OD частот у si_dpm/ci_dpm (зараз його немає ніде).
- **rtcore64.sys** — вбудований замість чужого (завантажувач служби з
  образом `RTCore64.sys`). Емуляція 17 IOCTL: rdmsr — лише датчики й довідкові
  (Intel `0x19C 0x1A2 0x198 0xCE 0xE7 0xE8 0x606 0x611 0x619 0x639`, AMD
  `0xC0010064+ 0xC0010293 0xC0010299 0xC001029B`); wrmsr — відмова; PCI
  config — читання з sysfs, запис лише індексу SMN (`00:00.0` 0x60) для
  температури Zen; порти `0xCF8/0xCFC` — як PCI config, інші — 0xFF;
  фізична пам'ять, BAR, читання/запис пам'яті ядра — відмова, доки траса не
  покаже потрібне читання (тоді — лише читання вузьких діапазонів BAR0 карти,
  що не спить).
- **Карта, що спить** (runtime PM): поки `runtime_status=suspended`, у RM
  не ходимо — повертаємо «спить»/останнє відоме, як Windows на Optimus.

## Без конфліктів із нашим «Тюнінгом»

1. **Один механізм.** «Тюнінг» для NVIDIA переходить з NVML
   (`GpcClk/MemClkVfOffset`, `SetGpuLockedClocks`) на ті самі функції, що й
   Afterburner: `SetPstates20`, `SetClockBoostTable`,
   `ClientPowerPoliciesSetStatus`, `SetClockBoostLock` — через той самий
   arctic-init. Два різні механізми зсуву (NVML і Pstates20) можуть
   додаватися — з одним механізмом цього питання немає. При оновленні
   arctic-init скидає зсуви NVML у 0 і переносить значення. AMD — і так ті
   самі файли sysfs.
2. **Джерело правди — карта.** Сторінка «Тюнінг» показує живі значення з
   карти (читання при відкритті й щосекунди), а не `games.ini`. Хто записав
   останнім — видно простим текстом під повзунком («Налаштовує MSI
   Afterburner»).
3. **Останнє застосоване, хто б не записав.** arctic-init пам'ятає для
   кожної карти останні значення і їх джерело. Повторне застосування (старт
   системи, старт гри, карта прокинулась — Linux може забути налаштування в
   D3cold) — **останніх** значень, ніколи — старих зі своїх налаштувань
   поверх Afterburner. Так розгін Afterburner переживає і сон карти.
4. **Хто останній — той і правий**, як між двома програмами на Windows.
   Afterburner застосовує свій профіль на своєму старті — arctic-init не
   перебиває.
5. **Захист від зависання** (`tuning-trial`) — для всіх записів, зокрема
   Afterburner: якщо попередній старт завис після розгону, записи цього
   старту тримаються, і сповіщення дає їх дозволити одним кліком.
6. **Процесор**: Afterburner його не розганяє; wrmsr через rtcore64 — відмова,
   тож андервольт і PL1/PL2 «Тюнінгу» ніхто не перебиває.
7. **Оверлей і клавіші**: коли RTSS показує OSD у грі (спільна пам'ять
   `RTSSSharedMemoryV2`, запис з PID гри), gameosd ховається — двох
   оверлеїв немає; клавішу, яку вже зайняли Afterburner/RTSS, gameosd не
   реєструє.
8. **Ліміт кадрів**: коли RTSS обмежує гру, панель «Ігри» пише це поруч із
   нашим лімітом; діє нижчий (DXVK `maxFrameRate` не вимикається сам).

## Усі функції: як кожна працює

| Функція | NVIDIA | AMD |
|---|---|---|
| Виявлення карти, назва, BIOS, драйвер | libnvidia-api ✅, dxvk-nvapi, клас дисплея, `nvldumd` | ADL AdapterInfo/VideoBiosInfo ← sysfs, `amdgpu.ids` |
| Зсув ядра | Pstates20 P0 ✅ / ClockBoostTable ✅ | OD_SCLK (OD8/ODN), GCN1/2 — патч |
| Зсув пам'яті | Pstates20 P0 домен 4 ✅ | OD_MCLK |
| Напруга (Voltage %) | VoltRails control ✅ | OD_VDDGFX_OFFSET (RDNA2–4), OD_VDDC_CURVE (Vega20, RDNA1), мВ станів (Polaris, Vega10), I2C VRM (старі плати) |
| Ліміт потужності | PowerPolicies ✅ | `power1_cap` |
| Ліміт температури, пріоритет | ThermalPolicies (ПК; мобільні — відмова драйвера) | OD8 target temp / `fan_target_temperature` |
| Редактор кривої V/F, Ctrl+L | ClockBoostTable/Mask/VFPCurve ✅, ClockBoostLock ✅ | OD_VDDC_CURVE (де є) |
| Вентилятори: ручний %, крива Afterburner | FanCoolers/FanPolicies (ПК), CoolerLevels (старі) | `pwm1`, `gpu_od/fan_ctrl/fan_curve` (RDNA3–4) |
| OC Scanner | траса (у 4.6.5 RTHAL окремих функцій сканера не видно) | — (як на Windows) |
| Датчики ГП: темп., hotspot, пам'ять, частоти, завантаження, потужність, вентилятор, напруга, причини обмежень | libnvidia-api ✅ (ThermChannel — маска каналів), D3DKMTQueryStatistics | PMLog ← `gpu_metrics` + hwmon |
| Датчики ЦП: темп., частота, потужність | rtcore64 MSR (Intel/AMD), SMN | те саме |
| ОЗП, файл підкачки, завантаження ЦП | Wine/ntdll | те саме |
| PerfCounter (PDH), SMART, Ping | PDH-лічильники з даних Arctic; SMART через ATA/NVMe passthrough; ICMP | те саме |
| HwInfo / AIDA64 | читають спільну пам'ять тих програм — працюють, коли ті працюють | те саме |
| Профілі, застосування на старті, синхронізація однакових карт | реєстр/файли; `StartupViaTaskScheduler` → taskschd | те саме |
| Гарячі клавіші (також у грі) | RegisterHotKey, DINPUT8 | те саме |
| Значок у треї з датчиками, скіни | user32/GDI | те саме |
| Logitech LCD | потрібне ПЗ Logitech (як на Windows) | — |
| RTSS: OSD у D3D9/10/11, D3D12, OpenGL, D3D8/DDraw | хуки на DXVK/vkd3d (+`D3D11On12CreateDevice`), wglSwapBuffers | те саме |
| RTSS: OSD і ліміт у Vulkan-іграх | неявний шар `VK_LAYER_RTSS` → потрібен завантажувач з шарами | те саме |
| RTSS: ліміт async / front / back edge | QPC + сон; `NtSetTimerResolution` — справжній | те саме |
| RTSS: Scanline Sync | `D3DKMTGetScanLine` з vblank dwm/KMS | те саме |
| RTSS: ліміт Reflex | dxvk-nvapi Reflex (VK_NV_low_latency2) | — (Reflex лише NVIDIA, як на Windows) |
| RTSS: frametime, 1 %/0,1 % low, бенчмарк, спільна пам'ять | у самому RTSS | те саме |
| Запис відео/скріншоти | CPU-кодеки ✅; NVENC ← `nvEncodeAPI64` на libnvidia-encode (docs/cuda.md) | QSV програмний ✅; AMF/VCE ← свій amfrt64 на Vulkan Video/VA-API |
| PresentMon/ETW (нові версії) | якщо траса покаже — ETW-сесії з подіями Present від dwm/DXVK | те саме |

## Межі, які лишаються

- **NVIDIA Kepler і старші** (GTX 600/700) — на nouveau, без NVAPI і без
  підняття частот (docs/M3-gpu.md). Датчики — частково; розгону як на
  Windows немає (гілка 470 не збирається з ядром 7.x).
- **AMD GCN1/2** — розгін лише після нашого патча si_dpm/ci_dpm.
- Те, що відмовляє сам драйвер для цієї карти (вентилятори EC ноутбуків,
  ліміт температури мобільних NVIDIA, напруга вище VBIOS), не працює й на
  Windows.

## Кроки

| # | Що | Перевірка | Стан |
|---|---|---|---|
| 1 | Траса останніх Afterburner/RTSS: ID NVAPI (лог невідомих ID), ADL, IOCTL RTCore, PresentMon/ETW, OC Scanner | лог | ⬜ |
| 2 | arctic-init: GPU-брокер (модель карт, білий список SET, межі, останнє застосоване + джерело, tuning-trial для всіх), «Тюнінг» NVIDIA → NVAPI | проба SET/GET, перезапуск, сон карти | ⬜ |
| 3 | nvapi/nvapi64: частина ГП над libnvidia-api (GET у процесі, SET через брокер), WoW64-переклад хендлів, DRS-сховище, I2C, `nvldumd` | Afterburner на 1050: зсуви, крива, Voltage %, ліміт | ⬜ |
| 4 | rtcore64.sys + підміна служби | датчики ЦП в Afterburner | ⬜ |
| 5 | D3DKMTQueryStatistics (адаптер/вузол/процес — з даних taskmgr), D3DKMTGetScanLine, NtSetTimerResolution | GPU.dll, Scanline Sync | ⬜ |
| 6 | vulkan-1: завантажувач Khronos над `winvulkan.json` (або шари у вбудованому) | RTSS/OBS/Steam у Vulkan-грі | ⬜ |
| 7 | atiadlxx/atiadlxy над amdgpu + `ppfeaturemask` в образі; патч GCN1/2 | карти AMD (див. 10) | ⬜ |
| 8 | PDH-лічильники, SMART, taskschd для автозапуску | плагіни, перезавантаження | ⬜ |
| 9 | Кодеки: NVENC, AMF, апаратний QSV | запис у RTSS | ⬜ |
| 10 | Перевірка на картах: NVIDIA Maxwell, Pascal (є), Turing+ (відкритий модуль + GSP), AMD Polaris, Vega, RDNA1–4 | матриця функцій вище | ⬜ |

Проби 21.11.2026 — `tools/nvapi-probe/` (`probe.c` GET, `probe2.c`/`probe3.c`
SET тих самих значень) над `libnvidia-api.so.1`; повторити на кожній карті
кроку 10 перед увімкненням запису для неї.
