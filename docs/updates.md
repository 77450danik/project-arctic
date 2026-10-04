# Оновлення, зібрані в самому Arctic

Мета (4.10.2026): працювати над Arctic з Arctic. Claude Code в Arctic міняє
код, збирає його там же й готує оновлення; воно ставиться під час
перезавантаження з екраном «Робота з оновленнями», як у Windows. До Windows 10
повертатися не треба ні для ReactOS, ні для Wine, ні для ядра.

## Що вже є (docs/persistence.md, «Оновлення»)

`C:\Windows\System32\Host\Update` з `host.sqfs` і `Boot\`, initrd ставить його
до старту, попередня версія — у `Host\Previous`, відкат з меню Limine.

Чого бракує:

1. Файлів C:\Windows. ReactOS (shell32, explorer, comctl32…) і все, що
   збірка кладе в префікс, лежать на C:, а не в `host.sqfs`; оновлення їх не
   чіпає.
2. Встановлення на GPT поряд із Windows: «bcdboot» (`write_esp`) шукає лише
   розділ EFI у MBR флешки й пише його корінь, а на SSD набір лежить у
   `\EFI\Arctic` на розділі EFI Windows (інший диск).
3. Збирати нема чим: в Arctic немає ні WSL, ні шелла для Claude Code
   (PowerShell — заглушка Wine, Git Bash не встановлено).

Усі три закрито (етапи 1–3 нижче).

## План

### Етап 1. Оновлення файлів Windows і GPT — зроблено 4.10.2026

- Пакет може мати будь-що з: `host.sqfs`, `Boot\`, `Windows\` (дерево, що
  лягає поверх `C:\Windows`, як `PendingFileRenameOperations`). `ready`
  пишеться останнім і перелічує, що в пакеті.
- Замінені файли `C:\Windows` ідуть у `Host\Previous\Windows`, тож відкат
  повертає й їх.
- `write_esp`: якщо C: на GPT, шукає розділ EFI з `\EFI\Arctic` на будь-якому
  диску й міняє лише цю теку (staging → rename).
- Перевірено у VM на диску як після встановлення (`make-install-test-disk.sh`,
  initrd з `make-dev-initrd.sh` через `INITRD=` у `make-install-set.sh`):
  пакет `Boot` + `Windows` (новий shell32.dll і новий файл) → «2 of 2 files
  of C:\Windows replaced», `\EFI\Arctic` переписано, робочий стіл з новим
  shell32; `vm-test.py --boot-disk … --append arctic.rollback=1` → старий
  shell32, доданий файл прибрано, `\EFI\Arctic` попередній.
- `install-2-write.ps1` і тестовий диск кладуть набір `\EFI\Arctic` у
  `C:\Windows\Boot\Arctic` (з образу там набір флешки): з нього оновлення й
  відкат пишуть розділ EFI. На SSD це треба один раз зробити руками.
- У меню Limine встановленого Arctic з'явився пункт **Arctic, undo the last
  update**, як на флешці.

### Етап 2. Підсистема Linux (WSL в Arctic) — зроблено 4.10.2026

Ті самі дистрибутиви, що у WSL Windows 10: `D:\WSL\arctic-build\ext4.vhdx`
(Arch: Wine, ядро, образ) і `D:\WSL\arctic-ros\ext4.vhdx` (Ubuntu: RosBE,
ReactOS). Одна збірка, той самий кеш ccache, де б не працював.

- **arctic-lxss** (`host/init/arctic-lxss.c`, root, як LxssManager; запускає
  arctic-init): на перший запит підключає VHDX через `qemu-nbd` (пакет
  qemu-img; модуль `nbd`), монтує ext4 у `/run/lxss/<ім'я>` з `/proc`,
  `/sys`, `/dev`, `resolv.conf` хоста і кожною літерою диска двічі: `/mnt/d`
  (як WSL) і `/d` (як Git Bash). Команда йде в `chroot`, у своїй сесії, як
  root або `-u`.
- **Список дистрибутивів:** `C:\ProgramData\Arctic\Lxss\distros`, рядок на
  кожен: `ім'я шлях-до-ext4.vhdx`, перший — типовий. Літери між стартами
  Arctic поки можуть мінятися місцями (запам'ятовування літер, як
  MountedDevices у Windows, ще немає), тож не знайдений за літерою VHDX
  шукається за тим самим шляхом на інших дисках.
- **wsl.exe, bash.exe, git.exe** (`runtime/wine/modules/programs/wsl`, одна
  програма Wine під трьома іменами в System32, як у Windows 10). Wine
  запускає ELF із CreateProcess сам, але без дескриптора процесу, коду
  виходу й stderr, тож клієнт — звичайна Windows-програма: TCP на 127.0.0.1
  (порт і токен — у `C:\ProgramData\Arctic\Lxss\session`; без токена сторінка
  в браузері до служби не достукається; AF_UNIX Wine для Windows-програм не
  має), запит, далі stdin/stdout/stderr кадрами, код виходу. Клієнт закрився
  — його команда вбивається (`kill` групи).
  - `wsl [-d ім'я] [-u користувач] [--cd тека] [-e cmd … | -- рядок | рядок]`,
    `wsl -l`, `wsl -t ім'я`, `wsl --shutdown`.
  - `bash.exe …` — bash типового дистрибутива, тека як у Git Bash (`/c/Users`).
  - `git.exe …` — git там, аргументи `C:\x` стають `/c/x`.
  - Консоль читається, лише коли натиснута клавіша із символом: читання,
    що лишилося б після виходу програми, у Wine з'їдало б наступні рядки cmd.
- **Claude Code:** `CLAUDE_CODE_GIT_BASH_PATH=C:\Windows\System32\bash.exe`
  у системному оточенні (`runtime/registry/wsl.reg`). Claude Code сам
  перетворює шляхи на `/c/…` і назад, `pwd -P` дає `/c/…`, бо диски — bind,
  а не посилання.
- **Вимкнення:** arctic-init спершу шле arctic-lxss SIGTERM і чекає до 60 с:
  процеси дистрибутивів завершуються, монтування знімаються, `qemu-nbd -d`
  дописує VHDX. Лише потім усе інше. arctic-lxss — subreaper для qemu-nbd
  (той після `--fork` осиротів би й лишився зомбі під arctic-init).
- arctic-volume не дає літер пристроям `nbd*`.
- Безпека: усе, що працює в Arctic, отримує root у дистрибутиві (як WSL з
  користувачем root). Для машини розробника так і задумано.

Перевірено у VM (диск як після встановлення + NTFS-диск з копією
`arctic-ros\ext4.vhdx`, `tests/vm/w1-wsl.txt`, `w2-wsl-shutdown.txt`,
`w3-wsl-read.txt`): `wsl -l`; `wsl -e uname -sr` → `Linux 7.2.6-arctic`;
`bash -c "pwd -P; …; exit 3"` з `C:\Users` → `/c/Users`, `exit=3`;
`git --version` → `git version 2.34.1`; `wsl -u nobody -e id`; `wsl
--shutdown`; «Завершення роботи» з командою, що працює в дистрибутиві →
`arctic-ros: detached`, `arctic-lxss stopped`, потім вимкнення; наступний
старт читає файл, записаний перед ним.

### Етап 3. Збирання оновлення — зроблено 4.10.2026

`tools/arctic/make-update.sh` (у дистрибутиві arctic-build: в Arctic через
`wsl -d arctic-build`, у Windows 10 — WSL з `--to /mnt/f`):

- `--windows`: файли `out/reactos` цього checkout, що відрізняються від
  `C:\Windows` (comctl32 — і в копії winsxs);
- `--host`: `host.sqfs` останньої збірки образу;
- `--boot`: ядро й initrd тієї збірки, підписані ключем розробника
  (`/root/arctic/out/secureboot-dev`, тобто на диску arctic-build) під цей
  C:: встановлений поряд із Windows — для GPT GUID з його `limine.conf`;
- `--live`: файли `C:\Windows` одразу (Linux міняє відкритий файл: процес
  лишається зі старим), explorer.exe перезапускається; старі — у
  `Host\Live\<час>`. Ще не перевірено.

Пакет додається до того, що вже чекає; `ready` — останнім. Перевірено у VM:
зіпсований `zipfldr.dll` на C: → «1 files differ», пакет `Windows` + `Boot`
→ старт ставить його, переписує `\EFI\Arctic`, файл знову як у збірці.

Як збирати в Arctic:

```
wsl -d arctic-ros -u root -- bash "/mnt/d/WORK_YT/project arctic/tools/wsl/build-reactos.sh"
wsl -d arctic-build -- bash "/mnt/d/WORK_YT/project arctic/tools/wsl/build-wine.sh"
wsl -d arctic-build -- bash "/mnt/d/WORK_YT/project arctic/tools/wsl/build-image.sh"
wsl -d arctic-build -- bash "/mnt/d/WORK_YT/project arctic/tools/arctic/make-update.sh" --windows --host --boot
```

потім перезавантаження.

### Реєстр після обриву живлення (RegBack)

Тест із «висмикнутим шнуром» (vm-test з `--boot-disk` раніше просто вбивав
QEMU) показав: обрив під час збереження реєстру міг коштувати обох кущів.
wineserver пише новий кущ поряд і перейменовує його поверх старого; до диска
перейменування не дійшло, а ntfsck на наступному старті прибрав обидва
імені. Wine стартував з порожнім реєстром: Windows 10 замість 11, вбудований
shell32 Wine замість ReactOS (ординал 200 — заглушка `SHLocalAlloc`),
explorer падав.

Як `config\RegBack` у Windows: коли робочий стіл хвилину працює і
`ver` каже 22000 (реєстр Arctic), arctic-init копіює кущі в
`C:\Windows\System32\config\RegBack`; на старті куща, якого немає або який
порожній, бере його звідти. Перевірено: кущі видалено → «system.reg was
lost, it came back from RegBack», «user.reg …», старт з реєстром Arctic.
vm-test тепер вимикає й встановлений диск кнопкою живлення.

### Етап 4. Як у Windows

- У меню живлення «Оновити та перезавантажити» / «Оновити та завершити
  роботу», коли пакет готовий.
- PowerShell для Claude Code: справжній PowerShell 7 замість заглушки Wine
  (або вимкнути інструмент PowerShell, коли є bash).

## Порядок

Етапи 1–3 зроблено з Windows 10 і ставляться на SSD востаннє вручну. Далі,
з Arctic, першими через новий шлях ідуть: IFileOperation (завантаження
Chrome) і діагностика згортання вікон (out/dialog-2026-10-04-arctic.md).
