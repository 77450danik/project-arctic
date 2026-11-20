# Збірка: CI і локально

Ніщо не збирається з нуля двічі: і в CI, і локально кожна частина бере
результати попередньої збірки й компілює лише те, що змінилось.

## CI (`.github/workflows/build.yml`)

| Частина | Готовий результат | Між прогонами | Зміна займає |
|---|---|---|---|
| Ядро | кеш `kernel-<хеш>` | ccache `kernel-ccache-*` | лише змінені файли |
| Wine | кеш `wine-<хеш>` | ccache `wine-ccache-*` | лише змінені файли й лінковка |
| ReactOS | кеш `reactos-<хеш>` | RosBE `rosbe-*` | ~2 хв (збираються лише цілі Arctic) |
| Образ | — | — | ~11 хв |

- Хеш береться від вхідних файлів частини: якщо вони не змінились, готовий
  результат береться з кешу без збірки.
- Якщо змінились — контейнер отримує ccache останнього прогону
  (`restore-keys`), а після збірки зберігає свій. Налаштування ccache спільні
  для CI і локальних збірок: `ci/ccache-env.sh`.
- Останнє завдання `prune-caches` видаляє старі кеші, щоб не вийти за 10 ГБ
  GitHub: з ccache лишається найновіший, з готових збірок — ті, якими
  користувались за останні два дні; RosBE лишається завжди.

## Локально (WSL на D:)

Дистрибутиви лежать на `D:\WSL`: `arctic-build` (Arch) і `arctic-ros`
(Ubuntu 22.04, для тулчейна ReactOS).

| Що | Команда | Де результат |
|---|---|---|
| Wine | `wsl -d arctic-build -- bash tools/wsl/build-wine.sh` | `/root/arctic/out/wine` |
| Ядро | `wsl -d arctic-build -- bash tools/wsl/build-kernel.sh` | `/root/arctic/out/kernel` |
| ReactOS | `wsl -d arctic-ros -u root -- bash tools/wsl/build-reactos.sh` | `out/reactos` |
| Образ | `hamam -d arctic-build -- bash tools/wsl/build-image.sh` (root на хості: `pacstrap`, `arch-chroot`, FUSE; `docs/linux-gate.md`) | `out/arctic-local.iso`, `out/arctic-usb.img` |

- Wine: дерево збірки `/root/arctic/wine` лишається між збірками, файли
  синхронізуються за вмістом, плюс ccache `/root/arctic/ccache`.
- Ядро: дерево розпаковується щоразу на тому самому місці, усе незмінне
  береться з ccache `/root/arctic/ccache-kernel`; архіви джерел качаються один
  раз на версію в `/root/arctic/downloads`.
- ReactOS: RosBE збирається один раз (близько години), далі ninja у
  `/root/reactos/build` перезбирає лише те, що зачепили патчі.
- Образ без локального ядра бере ядро з останнього ISO CI.

## Ім'я Wine

Людина не бачить імені Wine. `ci/wine-names.pl` проходить дерево Wine після
патчів:

| Було | Стало |
|---|---|
| `HKLM\Software\Wine`, `HKCU\Software\Wine` (і `Wow6432Node\Wine`) | `Software\Arctic\Runtime` |
| `…\Wine\WineDbg` | `…\Arctic\Runtime\Debugger` |
| ProductName файлів «Wine 11.18» | «Arctic» |
| FileDescription «Wine <модуль>» | «Arctic <модуль>» |
| «Wine HID device», «Wine HID compatible device» у диспетчері пристроїв | «HID-compliant device», «HID Input Device» |
| `Software\Wine\Time Zones\TZ Mapping` (kernelbase.rgs; за нею ntdll називає пояс хоста, сторінка «Дата й час» іде у зворотний бік) | `Software\Arctic\Runtime\Time Zones\TZ Mapping` |
| ProgID `WINEDBROWPRX.*`, `WINEDBRSTPRX.*` (msdaps) | `DBROWPRX.*`, `DBRSTPRX.*` |
| `winecfg.exe` і його IFEO в `dpi.reg` | прибрано; версію Windows 11 пише `runtime/registry/version.reg` |
| драйвери `winebus.sys`, `winehid.sys`, `winebth.sys`, `winexinput.sys`, `wineusb.sys`, їхні служби, процес `winedevice.exe` | `winbus.sys`, `winhid.sys`, `winbth.sys`, `winxinput.sys`, `windowsusb.sys` (`winusb.sys` — це WinUSB Windows), служби з тими самими іменами, `windevice.exe` |
| шини й пристрої `WINEBUS\…`, `WINEBTH\…`, `WINEXINPUT\…`, `ROOT\WINE\…`, сумісні ID `WINE_COMP_*`, група `WinePlugPlay` | `WINBUS\…`, `WINBTH\…`, `WINXINPUT\…`, `ROOT\WIN\…`, `WIN_COMP_*`, `WinPlugPlay` |
| програми й бібліотеки `wineboot`, `winedbg`, `winebrowser`, `winefile`, `winemine`, `winepath`, `winemenubuilder`, `wineconsole`, `winemsibuilder`, `wined3d`, `winevulkan` (і `winevulkan.json` ICD), `winewayland.drv`, `winedmo`, `winemapi` | `winboot.exe`, `windowsdbg.exe` (`windbg` — налагоджувач Microsoft), `winbrowser.exe`, `winfile.exe`, `winmine.exe` (так їх і звала Windows), `winpath.exe`, `winmenubuilder.exe`, `winconsole.exe`, `winmsibuilder.exe`, `wind3d.dll`, `winvulkan.dll` + `winvulkan.json`, `winwayland.drv`, `windmo.dll`, `winmapi.dll`; ім'я графічного драйвера, яке робочий стіл складає з частин (`"wine%s.drv"` у explorer і winsrv), — `"win%s.drv"` |

- Реєстр, зроблений раніше: arctic-init (`rename_wine_keys`) переносить ключі
  в тексті кущів до старту wineserver, один раз, коли реєстр відстає від
  wine.inf. Кущ, яким він був, лишається в `config\RegBack\<кущ>.wine`.
- Копірайт («the Wine project authors») лишається: код їхній.
- Модулі перейменовано цілком: каталоги й файли, а потім кожна згадка в
  дереві, у регістрі, як було написано (`winebus` → `winbus`, `WineBus` →
  `WinBus`, `WINEBUS` → `WINBUS`), і так само імена, записані масивами
  символів (`{'w','i','n','e','b','o','o','t',…}` — так ntdll запускає
  wineboot і називає його подію). Окремо виправлено те, що рахувало довжину
  старого імені: префікс `WINXINPUT\` у hidclass (10 символів замість 7),
  довжина ID у winbus (24 замість 25), кореневі пристрої в winboot
  (`root\win\…`). Ім'я каналу налагодження вміщує 14 символів: довший після
  перейменування губить префікс (каналів журналу людина не бачить).
  Змінна `WINEBOOTSTRAPMODE` теж стала `WINBOOTSTRAPMODE` — усюди однаково.
- Реєстр наявної системи arctic-init перейменовує в тому самому проході
  (`driver_names`, після старих правил для ключів: ключі, значення, `str(7)`
  з HardwareID, шляхи інтерфейсів `##?#WINBTH#…`), тож пристрої й служби
  лишаються, якими були. Ключі спарювання Bluetooth лежать у
  `Services\BTHPORT` і не змінюються. Старі файли (`drivers\wine*.sys`,
  `wine*.exe`/`.dll`/`.drv`, `inf\wine*.inf`, `winevulkan.json`) оновлення
  видаляє (`make-update.sh`, список `delete`). Журнал Bluetooth тепер пише
  `winbth:`.
- DXVK і vkd3d-proton шукають спершу `winevulkan.dll`, а не знайшовши —
  `vulkan-1.dll`, як на Windows; той веде у `winvulkan.dll`.
- З 16.11.2026 імена NT і в Linux-частині (кінець `ci/wine-names.pl`,
  установка в `ci/build-wine.sh`):

  | Було | Стало |
  |---|---|
  | `/usr/bin/wineserver` (процес сервера, синій екран `CRITICAL_PROCESS_DIED`) | `/usr/bin/ntexec` |
  | `/usr/bin/wine`, `…/x86_64-unix/wine`, `wine-preloader` | `ntloader`, `ntloader-preloader` |
  | `/usr/lib/wine`, `/usr/share/wine`, `wine.inf` | `/usr/lib/nt`, `/usr/share/nt`, `nt.inf` |
  | `/tmp/.wine-<uid>` (сокет сервера) | `/tmp/.nt-<uid>` |
  | змінні `WINEPREFIX`, `WINEDEBUG`, `WINEDLLOVERRIDES`, `WINELOADER`, `WINEDATADIR`, `WINECONFIGDIR`, `WINEHOMEDIR`, `WINEDLLDIRn`, `WINEUSERNAME`, `WINEUSERLOCALE`, `WINEUNIXCP`…, `WINE_HOST_*` (їх бачить кожна програма в `set`) | `NTPREFIX`, `NTDEBUG`, `NTDLLOVERRIDES`, `NTLOADER`, … , `NT_HOST_*` |
  | `WINE_LAZY_FLUSH`, `WINE_REGISTRY_SAVE_PERIOD`, `WINE_IMAGE_CACHE` | `NT_…` |
  | `winevdm.exe` (16-бітні програми) | `ntvdm.exe`, як у Windows |
  | `rosbrand.dll`; «ReactOS Project», «ReactOS Operating System» у версії файлів ReactOS | `basebrd.dll`; як у модулів Wine (ReactOS `0123`) |

  Збірка Wine лишається зі своїм розкладом (`make install` дає `lib/wine`),
  `build-wine.sh` перейменовує встановлене; `wine-names.pl` міняє лише
  рядки, за якими код шукає ці файли під час роботи. Що лишилося з іменем
  wine, збірка виводить рядками `still named wine:`.
- Не перейменовано те, чого програма не показує: інтерфейси `IWine*`
  (за ними знаходить себе 32/64-бітний маршалінг), інструменти збірки
  (`winebuild`, `winegcc`, `wrc`, обгортка `tools/wine` у дереві збірки),
  процесор друку `wineps`, `winealsa.drv` (`Drivers\winealsa.drv` — GUID
  аудіопристроїв; ім'я звукового драйвера mmdevapi складає з частин).
- ReactOS: патч `0085` (опис comctl32, browseui, shdocvw, CompanyName без
  «Wine Team»).
- Патч `0098`: ntdll більше не експортує `wine_get_version` (і
  `wine_get_build_id`, `wine_get_host_version`). Програма, що питає їх у
  ntdll — звичайна перевірка «це Wine?», якою користується й OBS, — не
  знаходить нічого, як на Windows. Самі функції лишаються в version.c для
  частин Wine (вони беруть їх через `GetProcAddress` і обходяться без них).
