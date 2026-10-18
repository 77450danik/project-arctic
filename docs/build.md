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
| Образ | `wsl -d arctic-build -- bash tools/wsl/build-image.sh` | `out/arctic-local.iso`, `out/arctic-usb.img` |

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

- Реєстр, зроблений раніше: arctic-init (`rename_wine_keys`) переносить ключі
  в тексті кущів до старту wineserver, один раз, коли реєстр відстає від
  wine.inf. Кущ, яким він був, лишається в `config\RegBack\<кущ>.wine`.
- Копірайт («the Wine project authors») лишається: код їхній.
- Не перейменовано те, чого програма не показує і за чим частини
  знаходять одна одну: файли `winebus.sys` тощо, шина `WINEBUS`, група
  `WinePlugPlay`, інтерфейси `IWine*`, `/usr/share/wine` на боці Linux.
- ReactOS: патч `0085` (опис comctl32, browseui, shdocvw, CompanyName без
  «Wine Team»).
- Патч `0098`: ntdll більше не експортує `wine_get_version` (і
  `wine_get_build_id`, `wine_get_host_version`). Програма, що питає їх у
  ntdll — звичайна перевірка «це Wine?», якою користується й OBS, — не
  знаходить нічого, як на Windows. Самі функції лишаються в version.c для
  частин Wine (вони беруть їх через `GetProcAddress` і обходяться без них).
