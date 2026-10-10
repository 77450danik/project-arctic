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

## Центр мереж і параметри TCP/IP (Windows 7)

`netcenter.dll` (`runtime/wine/modules/dlls/netcenter`, CLSID `{8E908FC9-…}`) —
сторінка Панелі керування в Провіднику, як System і Power Options (спільний
каркас `systemcpl/cpanel.c`, `cpfolder.c`):

- **Центр мережевих підключень і спільного доступу**: схема «комп'ютер — мережа
  — Інтернет» (хрестик там, де зв'язку нема), активні мережі (назва/SSID, тип
  доступу, підключення → його «Стан»), завдання. Відкривається з меню значка
  в треї, з посилання флайаута, з «Властивостей» Мережі, з Панелі керування.
- **Стан підключення** (`rundll32 netcenter.dll,ShowConnectionStatus {GUID}` або
  ім'я інтерфейсу): досяжність IPv4/IPv6, стан носія, тривалість, швидкість
  (Wi-Fi — бітрейт з `StationDiagnostic` iwd), SSID і сигнал, байти;
  «Докладно…», «Властивості», «Вимкнути/Увімкнути», «Діагностика» (DHCP ще
  раз / мережа Wi-Fi знову).
- **Властивості підключення**: адаптер, компоненти з прапорцями; зняти IPv4 чи
  IPv6 — вимкнути протокол на адаптері.
- **Властивості IPv4 і IPv6** — діалоги Windows (tcpipcfg.dll, netshell.dll:
  `tools/res2rc.py` → `dialogs_uk.rcinc`): адреса автоматично або вручну (маска
  / довжина префікса, шлюз), DNS автоматично або вручну, «Альтернативна
  конфігурація», «Додатково…» (кілька адрес, шлюз, порядок DNS, NetBIOS).

Папка «Мережні підключення» (netshell, патч ReactOS 0066) відкриває ці ж
«Стан» і «Властивості».

### Як параметри доходять до хоста

Параметри інтерфейсу — файл `/run/arctic/network/<інтерфейс>.conf` (на диску —
`…\config\Host\network`, переживає перезавантаження), який пише unix-частина
`netcenter.so`:

```
enabled=1
ipv4=dhcp | static | off     address=192.168.0.10/24   gateway=192.168.0.1
dns=8.8.8.8 1.1.1.1          (порожньо — з DHCP)
ipv6=auto | static | off     address6=2001:db8::5/64   gateway6=fe80::1
dns6=…
repair=<лічильник>           («Діагностика»: DHCP ще раз)
```

- `arctic-init` стежить за текою (inotify) і застосовує одразу, як Windows:
  дротовий порт — `udhcpc` або адреси й маршрут вручну (`busybox ip`), вимкнене
  підключення — `link down`; IPv6 будь-якого інтерфейсу — `disable_ipv6`,
  `accept_ra`, `autoconf`, адреси вручну. Порт, який уперше з'явився, одразу
  бере свої параметри.
- DNS вручну підставляє `arctic-resolv`: якщо в параметрах інтерфейсу є `dns=`
  чи `dns6=`, вони йдуть у `resolv.conf` замість отриманих від DHCP/iwd.
- Wi-Fi адресує iwd, тож адреси вручну `netcenter` пише секціями `[IPv4]` /
  `[IPv6]` у файли мереж iwd (і в `wifi.iwd`, звідки `wlanapi` додає їх до
  мереж, приєднаних пізніше) та приєднує мережу знову.
- Те саме дзеркалиться в `Tcpip\Parameters\Interfaces\{GUID}` (EnableDHCP,
  IPAddress, SubnetMask, DefaultGateway, NameServer), як у Windows.

Ім'я комп'ютера: `arctic-init` бере його з `Hostname` у `Tcpip\Parameters`
(`system.reg`) перед стартом Wine — те, що задано в «Властивості системи →
Ім'я комп'ютера» (sysdm.cpl + netid.dll ReactOS, патч 0067), діє з наступного
запуску.

## Альтернативна конфігурація

Вкладка «Альтернативна конфігурація» IPv4 (`alternate=user|apipa …` у
`IF.conf`) тепер застосовується, як у Windows: arctic-init (`serve_alternate`)
дивиться на кожен інтерфейс у режимі DHCP; якщо він підключений, а IPv4 немає
довше 15 с, ставить адресу користувача (з шлюзом і DNS) або APIPA
169.254.x.y/16 (з MAC). Щойно DHCP дав адресу (або з'явилась друга), своя
знімається.

## Мережеве оточення («Мережа» в Провіднику)

Як у Windows: «Мережа» — комп'ютери мережі, комп'ютер — його спільні папки,
папка — файли; `\\сервер\папка` відкривається звідусіль (адресний рядок,
«Виконати», програми), диски підключаються.

| Що | Де |
|---|---|
| Мережеві папки на хості | `host/init/arctic-smb.c`: autofs на `/run/arctic/unc` (`dosdevices/unc` → туди); перше звернення до `\\сервер` дає теку сервера (теж autofs), до `\\сервер\папка` — монтує її ядерним SMB-клієнтом (`cifs.ko`) з іменем і паролем, які дав Windows-бік, або як гість. Імена в нижньому регістрі. Сервер шукається DNS, LLMNR, NetBIOS, mDNS. Керування — TCP 127.0.0.1, порт і токен у `C:\ProgramData\Arctic\Smb\session` (`CRED/GETCRED/HOST/RESOLVE/MOUNT/UMOUNT/LIST`). |
| Мережевий провайдер Windows | `runtime/wine/modules/dlls/ntlanman` («Microsoft Windows Network», `NetworkProvider` у `runtime/registry/netfolders.reg`): NP*-API для `mpr.dll`; пошук комп'ютерів (`discover.c`: WS-Discovery, NetBIOS, mDNS `_smb._tcp`); список папок (`smb2.c`: SMB 2.0.2/2.1, NTLMv2 у SPNEGO, підпис, `srvsvc` NetrShareEnum через DCE/RPC); запит пароля CredUI, «Запам'ятати» — у Credential Manager; «Підключення мережевого диска», «Відключення мережевих дисків» (діалоги `netplwiz.dll.mui`); `RestoreConnections` при вході. |
| mpr | патч Wine 0081: `CONNECT_INTERACTIVE` доходить до провайдера, `WNetConnectionDialog1`/`WNetDisconnectDialog` — діалоги провайдера, `WNetGetUniversalName`. |
| Папка «Мережа» | патч ReactOS 0068 (`CNetFolder`): ієрархія комп'ютери → папки → `CFSFolder`, стовпець «Примітки», розбір `\\сервер\папка\…`, «Підключити мережевий диск…» у меню папки, `SHStartNetConnectionDialogW`. Дієслова «Підключити/Відключити мережевий диск» у «Цей ПК» і «Мережа» (`netfolders.reg`). |
| Автодоповнення адреси | патч ReactOS 0069: `CACListISF` (browseui) — потік підказок і потік вікна по черзі (повільна мережева папка оголила гонку, explorer падав на `\\сервер`). |
| Параметри | Центр мереж → «Змінити додаткові параметри спільного доступу» (`netcenter/page_sharing.c`): мережеве виявлення (`HKLM\SOFTWARE\Arctic\Network\NetworkDiscovery`), мережеві диски, «Забути збережені мережеві паролі». |

Перевірено (10.10.2026) у WSL з Samba 4: список папок гостем і з паролем
(зокрема з обов'язковим підписом), відмова з неправильним паролем,
`\\SAMBATEST` через NetBIOS, знайдено `SAMBATEST` (NetBIOS), `WSDBOX` (wsdd,
WS-Discovery), `avahibox` (mDNS); autofs монтує `\\127.0.0.1\public` при
першому `ls`.
У ВМ (10.10.2026, образ з усіма латками, Samba у WSL через 10.0.2.2):
`\\10.0.2.2` — private, public, «Папка»; public гостем одразу; private —
запит CredUI, після пароля відкривається; «Підключити мережевий диск…» з
меню папки → «public (\\10.0.2.2) (Z:)» у «Цей ПК»; сторінка додаткових
параметрів; APIPA 169.254.x.y на карті без DHCP через ~16 с.

## Перевірено

- QEMU (e1000, user-mode): DHCP 10.0.2.15, DNS 10.0.2.3, HTTP до
  example.com; значок у треї, підказка «Мережа / Доступ до Інтернету», список.
  `tests/vm/m5-network-tray.txt`.

## Відкрите

- Wi-Fi — лише на залізі (QEMU не має бездротової карти).
- Центр мереж: домашня група, категорія мережі (домашня/робоча/громадська) —
  немає; мережі завжди «приватні». WINS/NetBIOS зберігаються, але не діють.
- Спільний доступ **з** Arctic (свої папки й принтери для інших комп'ютерів,
  видимість Arctic у мережі) — немає: потрібен SMB-сервер (ksmbd з його
  `ksmbd.mountd` — Linux-програма користувача, якої в образі бути не може).
- Пошук комп'ютерів — лише IPv4; домени й Active Directory — немає.
- Підказка трею біля правого краю екрана обрізається.
