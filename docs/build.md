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
