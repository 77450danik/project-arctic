# M5 — Мережа: робочі нотатки

Як Arctic виходить в інтернет і що бачить користувач. Документ робочий.

## Linux-бік (arctic-init)

- **Кабель.** На кожен дротовий інтерфейс (`/sys/class/net/*` без `wireless/` і
  `phy80211`) arctic-init запускає `busybox udhcpc -f -i IF`; перевіряє раз на
  10 с, тож кабель, вставлений пізніше, теж підхоплюється. Скрипт
  `/usr/lib/arctic/udhcpc.script` ставить адресу й маршрут через `busybox ip`.
- **Wi-Fi.** Системна шина `dbus-daemon --system` і `iwd` з
  `EnableNetworkConfiguration=true` (адреси по DHCP видає сам iwd).
  Каталог стану iwd — `/run/arctic/iwd`, змонтований на `/var/lib/iwd` і
  належний `nt`: `wlanapi.dll` кладе туди `<ssid>.psk`. Після перезавантаження
  паролі живуть лише в реєстрі (`HKLM\Software\Microsoft\Wlansvc\Profiles`).
- **DNS.** Свій `resolvconf` (`host/init/arctic-resolv.c`): udhcpc та iwd
  пишуть сервери по інтерфейсу в `/run/arctic/resolv.d/IF`, з них збирається
  `/run/arctic/resolv.conf`, на який вказує `/etc/resolv.conf`.
- Доступ `nt` до `net.connman.iwd` — `/etc/dbus-1/system.d/arctic.conf`.

## Windows-бік

| Що | Де | Звідки |
|---|---|---|
| `wlanapi.dll` | `runtime/wine/modules/dlls/wlanapi` | наш, sd-bus → iwd |
| Значок мережі в треї (Windows 7) | `runtime/wine/modules/dlls/pnidui` | наш, CLSID `{7007ACCF-…}`, його запускає stobject |
| Папка «Мережні підключення» (Windows XP) | ReactOS `netshell.dll`, патч 0017 | ReactOS |
| Пуск → Настройка → Мережні підключення | ReactOS shell32, розгортається в меню | ReactOS |

- `wlanapi`: інтерфейси, скан, список мереж, профілі (XML як у Windows),
  підключення/відключення, сповіщення ACM (скан, підключено, помилка,
  відключено). GUID інтерфейсу — з імені інтерфейсу.
- `pnidui`: значок малюється сам (монітор для кабелю, смужки для Wi-Fi,
  хрестик/жовтий знак), список мереж з кнопкою «Підключитися», діалог ключа.
  `rundll32 pnidui.dll,ShowNetworkList` відкриває цей список з інших місць.
- `netshell`: з'єднання — це інтерфейси з `GetAdaptersInfo` (Ethernet і
  802.11), а не клас Plug and Play; імена «Підключення по локальній мережі»,
  «Бездротове мережне підключення», перейменування пишеться в
  `Control\Network\{4D36E972-…}\{GUID}\Connection`. Вікно «Стан» — netshell,
  але без власних значків у треї. Для бездротового з'єднання є
  «Перегляд доступних бездротових мереж» (дія за замовчуванням, коли не
  підключено) — це список pnidui.

## Перевірено

- QEMU (e1000, user-mode): DHCP 10.0.2.15, DNS 10.0.2.3, HTTP до
  example.com; значок у треї, підказка «Мережа / Доступ до Інтернету», список.
  `tests/vm/m5-network-tray.txt`.

## Відкрите

- Wi-Fi — лише на залізі (QEMU не має бездротової карти).
- Властивості з'єднання: список компонентів порожній (netcfgx Wine), статичну
  адресу поки не задати.
- «Вимкнути» з'єднання не працює (шлях SetupDi).
- Підказка трею біля правого краю екрана обрізається.
