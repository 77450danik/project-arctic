# Швидкий запуск і гібернація (6.10.2026)

Користувач: «опції і fast start, і гібернації мають бути, але вимкнені за
замовчуванням і вмикатися через msconfig»; «зроби так само безпечно, з тими
самими стандартами безпеки, що й у Windows».

## Що це

- **Швидкий запуск** (Fast Startup, `HiberbootEnabled`): «Завершення
  роботи» та звичайне «Перезавантаження» без оновлень закривають сеанс
  (вихід користувача, програми закриваються, як і зараз), а ядро Linux з
  драйверами й системна частина NT (wineserver, служби, winedevice)
  засинають у `C:\hiberfil.sys`. Старт: initrd відновлює образ, arctic-init
  запускає новий сеанс (wininit → dwm → winlogon → explorer). (Windows
  робить це лише на «Завершення роботи»; користувач хоче й на
  перезавантаження без оновлень.)
- **Гібернація** (`HibernateEnabled`): пункт «Гібернація» в меню живлення;
  засинає все, разом із відкритими програмами.

Обидва вимкнені за замовчуванням; вмикаються в msconfig (зібрати з ReactOS
`base/applications/msconfig`, додати прапорці). Ключі як у Windows:
`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power\HiberbootEnabled`,
`HKLM\SYSTEM\CurrentControlSet\Control\Power\HibernateEnabled`.

## Місце в коді

- wininit.exe завершується з кодом 3 (перезавантаження) / 4 (вимкнення),
  коли сеанс уже закрито, а wineserver і служби живі → `child_exited` →
  `power_off()` у `host/init/arctic-init.c`. Швидкий запуск — тут:
  замість `power_off` заснути, після пробудження знову `wininit.exe`.
- Ядро: `CONFIG_HIBERNATION=y`, `CONFIG_HIBERNATION_SNAPSHOT_DEV=y`
  (`/dev/snapshot`, як uswsusp): образ пише й читає наш код, у файл на C:.

## Безпека (як у Windows)

1. Образ перевіряється: підпис, контрольна сума кожного блоку, збірка ядра
   (`uname -v`), PARTUUID C:. Не сходиться — образ відкидається, старт
   холодний (у Windows — «Видалити дані відновлення»).
2. Одноразовість: initrd позначає заголовок використаним ДО відновлення.
3. Поки Arctic спить, його C: схований від Windows 10: тип розділу в GPT
   (основна і резервна таблиці, CRC) тимчасово «Arctic hibernated»; Windows
   не монтує такий розділ. initrd повертає Basic Data на кожному старті.
4. Диски Windows (D:, E:, F: з WSL) перед сном: sync + umount. Зайнятий —
   швидкий запуск робить звичайне вимкнення, гібернація відмовляє з назвою
   диска.
5. Оновлення чекає (`Host\Update\ready`), меню Limine, «nothing saved»,
   флешка — без гібернації.
6. initrd читає образ сирими секторами (екстенти `hiberfil.sys`, записані
   в заголовок і в змінну EFI), C: до відновлення не монтує.

## Етапи

1. Сон/пробудження в arctic-init + initrd, перевірка в VM (KVM у
   arctic-build, вікно через noVNC у Chrome).
2. Швидкий запуск від кода wininit 3/4.
3. Гібернація: пункт меню живлення (explorer), запит до arctic-init.
4. msconfig з ReactOS + прапорці.

## Як зроблено

- `host/init/hiberfil.{c,h}` — спільне для arctic-init і initrd: формат
  (перший МіБ файла — заголовок, екстенти, CRC-32C кожних 4 МіБ; далі образ
  ядра з `/dev/snapshot`), зміна типу розділу в GPT (резервна таблиця
  першою, CRC обох), змінна EFI `ArcticHiberfil-7a5e5b3c-…` (де заголовок).
- arctic-init, засинання (`sleep_to_disk`): `C:\hiberfil.sys` на 2/5
  пам'яті через `fallocate` (миттєво), FIEMAP → сектори на диску;
  `/sys/power/image_size`; спершу нульова сторінка заголовка; `sync`,
  `drop_caches`; `SNAPSHOT_FREEZE` → `SNAPSHOT_CREATE_IMAGE`; далі ФС не
  пишуться — образ іде O_DIRECT у сектори файла через цілий диск,
  заголовок останнім, тоді тип C: «asleep», вимкнення/перезавантаження.
  NVIDIA: `/proc/driver/nvidia/suspend` «hibernate»/«resume»
  (`NVreg_PreserveVideoMemoryAllocations=1`, копія у /tmp — у пам'яті,
  тож в образі; /var/tmp тут лише для читання).
- Швидкий запуск: код wininit 3/4 → `fast_startup()`: `HiberbootEnabled`
  через reg.exe (wineserver ще живий), без оновлення в черзі; екран
  «Завершення роботи», WSL зупиняється, процеси сеансу завершуються (усі
  NT-процеси, крім wineserver, services.exe, winedevice, plugplay, svchost,
  rpcss, spoolsv — Wine запускає процеси подвійним fork, батько кожного —
  arctic-init, тож лише за назвою), диски Windows від'єднуються. Після
  пробудження: годинник з RTC як місцевий час, SNTP, диски знову (udev
  add), WSL, новий сеанс (`start_session`). bootanim з командою `resume`
  сам повертає екран старту, коли BOOTTIME обганяє MONOTONIC.
- Гібернація: «Гібернація» в меню живлення startui (коли
  `HibernateEnabled`) → `powrprof!SetSuspendState(TRUE)` (модуль powrprof, docs/power.md)
  → `/run/arctic/power/request` → `serve_power()`: WSL стоп, диски Windows
  (зайнятий — відмова, startui пише чому), сон; після пробудження
  `udevadm trigger --action=change --subsystem-match=drm` — dwm.exe
  перемальовує все (пам'ять відеокарти VM/дискретної в образ не потрапляє).
- initrd (`wake_or_forget`, до будь-якого монтування C:): тип C: «asleep»?
  → змінна EFI, заголовок, перевірки (CRC області, збірка ядра, PARTUUID,
  екстенти) → позначка «використано» → тип Basic Data назад → образ у
  `/dev/snapshot` з перевіркою CRC кожного шматка → `SNAPSHOT_ATOMIC_RESTORE`.
  Щось не так — «no wake (…): a full start», як після вимкнення живлення.
- Кнопка живлення в перші 5 с сеансу ігнорується (winsrv, за часом події
  libinput): натискання, що ввімкнуло ПК.
- msconfig (ReactOS, патч 0043): вкладка «Завантаження» — «Arctic
  (C:\Windows)» і «Параметри живлення» з двома прапорцями.

## Перевірено у VM (KVM у arctic-build, вікно через noVNC у Chrome)

Тестовий диск: `make-install-test-disk.sh` + прапорці в system.reg.
- Швидкий запуск: кнопка живлення → образ 202 МБ → вимкнення; старт →
  «C: is asleep», «waking: 202 MB», новий сеанс, робочий стіл.
- Гібернація (тестова програма з ключа Run викликає SetSuspendState) →
  образ ~410 МБ → пробудження в той самий сеанс.
- Окремо знайдено: програма в папці «Автозавантаження» (StartUp) валить
  explorer.exe — він бере вбудований shell32 Wine (`SHLocalAlloc`); через
  ключ Run — без проблем. Не пов'язано з гібернацією.

На ноутбуці ще не перевірено (NVIDIA 580, i915, справжній SSD).
