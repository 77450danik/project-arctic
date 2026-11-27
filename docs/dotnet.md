# .NET Framework

Windows 11 має .NET Framework 4.8 від початку, і програми на ньому (WPF,
WinForms, інсталятори на кшталт Logitech G HUB) розраховують, що він є.
В Arctic це справжній .NET Framework від Microsoft, не Mono.

## Як він стоїть

- **Образ**: `ci/install-dotnet.sh` качає офлайн-інсталятор
  `NDP48-x86-x64-AllOS-ENU.exe` (перевірка SHA-256, як для інших
  Windows-програм образу), дістає з нього `netfx_Full_x64.msi` +
  `netfx_Full.mzz` і ставить MSI в C: власним `msiexec` образу. Це той самий
  пакет, який інсталятор ставить на Windows 7/8.1; на 10 і 11 ті самі файли
  йдуть із системою. У репозиторії бінарників Microsoft немає.
- **`mscoree.dll`** — файл системи, не фреймворку: з Windows 8 це шим, який
  передає кожен експорт у `mscoreei.dll` встановленого фреймворку. Пакет
  фреймворку для 8+ його не містить. В Arctic це `mscoree` Wine з патчем
  0156: якщо є `InstallRoot\v4.0.30319\mscoreei.dll`, кожен експорт стрибає
  в mscoreei (аргументи не чіпаються, тож один `jmp` підходить до будь-якої
  сигнатури), а DllMain повідомляє mscoreei через `OnShimDllMainCalled`.
  Без фреймворку лишається власний код Wine.
- **`InstallRoot`** (`runtime/registry/dotnet.reg`) — у Windows його пише
  системний mscoree; шим без нього бере `%SystemRoot%\Microsoft.NET\Framework{64}\`.

## Ординали шиму

Нумерація експортів `mscoree` така сама, як у `mscoreei` (x64), крім
`_CorExeMain2`, чий номер 142 зайнятий, і з двома безіменними, про які знає
тільки сам фреймворк:

| ординал | хто кличе | що це |
|---|---|---|
| 24 | `clr.dll`: `GetProcAddress(mscoree, 24)`, далі `f(&IID {F4D25DF3-E9B3-439C-8B2B-C814E36F9404}, &out)` | інтерфейс шиму для рантайму, іде в `mscoreei` #24 |
| 142 | `mscoreei.dll` зі свого DllMain, структура `0x114` байтів | справжня версія ОС (`OSVERSIONINFOW`, `RtlGetVersion`) |

Як знайдено (2026-11-25): без #24 у Wine під цим номером стояв
`CallFunctionShim`, і CLR падав у `clr!…` з `80131506` (читання NULL після
«успішного» виклику); без #142 mscoreei стрибав у нуль. Інструменти:
налагоджувач на ctypes (Debug API: завантаження DLL, винятки, регістри,
скан стеку), публічні PDB із `msdl.microsoft.com` + `llvm-symbolizer` /
`llvm-pdbutil` / `llvm-objdump` у arctic-build.

## Перевірка

- `RegAsm.exe /?` з `Framework64\v4.0.30319` друкує довідку 4.8.3761 (код 0).
- Інсталятор G HUB (WPF, .NET 4.7.2) показує вікно, качає й розпаковує G HUB.

## Logitech G HUB від початку до кінця

Що знадобилось, щоб звичайний `lghub_installer.exe` поставив G HUB і той
побачив мишку (G102, 2026-11-25), по порядку, як воно спливало:

| крок | що було | виправлення |
|---|---|---|
| інсталятор (WPF) | немає .NET | .NET 4.8 в образі, `mscoree` шим (Wine 0156) |
| агент стартує | `PssWalk*` немає → c0000135 | Wine 0157 |
| | `MFSENSORGROUP.dll` немає | модуль `mfsensorgroup` |
| | `ApiInformation::IsTypePresent` → E_NOTIMPL → виняток C++/WinRT | Wine 0158 |
| агент як служба | користувацька служба (тип 0x50) відхилялась SCM | Wine 0159: шаблони й екземпляри `<ім'я>_<LUID>` на `WinSta0\Default`, тайм-аут 30 с |
| мишка | колекція HID++ (0xFF00) інтерфейсу MI_01 губилась через evdev | Wine 0160: інтерфейс з вендорською колекцією береться з hidraw |
| усе після перезавантаження | реєстр не потрапляв на диск з 23.11 | ntos `save_target` (`kernel/drivers/ntos/crt.c`): посилання вулика шукається від кореня |

Перевірено наживо: ping HID++ (`11ff001a…` → протокол 4.2), G HUB показує
G102 LIGHTSYNC, DPI і частоту опитування, вхід в акаунт Logi ID.

Відкрите:
- один раз агент упав у `ucrtbase!fpnum_parse` на локалі, звільненій
  `setlocale` в іншому потоці (гонка глобальної локалі CRT Wine); більше
  не повторилось;
- `logi_lamparray_driver_installer`: «This OS is not Windows 11» — драйвер
  LampArray (Dynamic Lighting) хоче 22H2+, Arctic каже 22000;
- щойно видалені DLL (`libEGL.dll` тощо) лишаються «на видаленні» до
  перезавантаження, хоча їх не тримає жоден процес (кеш образів ntos?).
