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

### Етап 2. Підсистема Linux (WSL в Arctic)

Ті самі дистрибутиви, що у WSL Windows 10: `D:\WSL\arctic-build\ext4.vhdx`
(Arch: Wine, ядро, образ) і `D:\WSL\arctic-ros\ext4.vhdx` (Ubuntu: RosBE,
ReactOS). Одна збірка, той самий кеш ccache, де б не працював.

- `host.sqfs`: `qemu-nbd` (пакет qemu-img), модуль `nbd`.
- arctic-init (root) підключає VHDX через `qemu-nbd`, монтує ext4 у
  `/run/lxss/<ім'я>` з `/mnt/c`, `/mnt/d`… (як WSL), `/proc`, `/sys`, `/dev`.
- Служба (як LxssManager): сокет Unix, клієнт передає stdin/stdout/stderr
  (SCM_RIGHTS), каталог, argv, середовище; служба робить chroot у
  дистрибутив і запускає команду.
- `wsl.exe` і `bash.exe` у System32 — ELF-клієнт цієї служби. Wine запускає
  ELF із CreateProcess сам (`fork_and_exec` у ntdll), стандартні дескриптори
  підключені.
- Claude Code: `CLAUDE_CODE_GIT_BASH_PATH` → `bash.exe`; у дистрибутиві
  `/c`, `/d`… → `/mnt/c`… і `cygpath`, як у Git Bash.
- Безпека: усе, що працює в Arctic, отримує root у дистрибутиві (як WSL з
  користувачем root). Для машини розробника так і задумано.

### Етап 3. Збирання оновлення

`tools/arctic/update.sh` (у дистрибутиві): збирає те, що змінилося
(ReactOS — ninja, Wine — make, ядро, `host.sqfs`), підписує набір завантаження
ключем розробника з D:, складає пакет у `C:\Windows\System32\Host\Update`.
Те, що можна замінити наживо (explorer.exe), — одразу, з перезапуском.

### Етап 4. Як у Windows

- У меню живлення «Оновити та перезавантажити» / «Оновити та завершити
  роботу», коли пакет готовий.
- PowerShell для Claude Code: справжній PowerShell 7 замість заглушки Wine
  (або вимкнути інструмент PowerShell, коли є bash).

## Порядок

Етапи 1–2 робляться з Windows 10 і ставляться на SSD востаннє вручну. Далі,
з Arctic, першими через новий шлях ідуть: IFileOperation (завантаження
Chrome) і діагностика згортання вікон (out/dialog-2026-10-04-arctic.md).
