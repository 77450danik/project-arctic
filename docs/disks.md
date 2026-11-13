# Диски для програм Windows: сирий доступ, розділи, форматування

Мета: стокові програми, що працюють із дисками напряму (перша — Rufus 4.15),
працюють в Arctic як у Windows. Програми не патчимо.

## Що робить Rufus (src/dev.c, drive.c, format.c, 4.15)

- Список: `SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK)`, перелічувач `USBSTOR`,
  `SPDRP_HARDWAREID`/`FRIENDLYNAME`/`REMOVAL_POLICY`, шлях інтерфейсу → `CreateFile`
  → `IOCTL_STORAGE_GET_DEVICE_NUMBER`. VID:PID — з батька `USB\VID_xxxx&PID_xxxx`
  через `CM_Get_Device_ID_List("USBSTOR")`, `CM_Locate_DevNode`, `CM_Get_Child`.
- Стартує, лише якщо є `System32\bcdboot.exe` із версією (інакше NULL і падіння).
- Запис: `\\.\PhysicalDriveN` (`SetFilePointerEx`/`WriteFile`), `IOCTL_DISK_CREATE_DISK`,
  `IOCTL_DISK_SET_DRIVE_LAYOUT_EX`, `IOCTL_DISK_UPDATE_PROPERTIES`,
  `FSCTL_LOCK_VOLUME`/`DISMOUNT`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`.
- Форматування: `fmifs!FormatEx` за шляхом `\\?\Volume{GUID}` (NTFS, exFAT, FAT);
  FAT32 понад 32 ГБ і ext — власним кодом Rufus через сирий том.
- Без VDS Rufus іде старим шляхом (`need_logical`, без блокування диска).
  VDS не робимо.

## Як влаштовано

**Хто що може.** Увесь світ NT — користувач `nt`. `arctic-volume` віддає `nt`
лише вузли USB-дисків, яких не використовує сам хост (не C:, не носій старту).
Внутрішні диски й диск із системою сирим записом недоступні. Linux-програм
для цього в `host.sqfs` не додаємо: форматування — у `fmifs.dll`, Windows-коді.

**Хост.** `arctic-volume` описує диск у `/run/arctic/disks/<ім'я>`, а кожен
розділ такого диска — у `/run/arctic/volumes/<ім'я>`, навіть без файлової
системи (тоді `mount=` порожній). Служба `arctic-disk` (root, сокет
`/run/arctic/disk.sock`, лише для `nt`) робить те, що потребує привілеїв:
`rescan` (BLKRRPART), `dismount`, `mount`. Файл `/run/arctic/hold/<ім'я>`
забороняє автомонтування, поки том заблоковано.

**mountmgr.sys.** Для диска — `\Device\HarddiskN\DR0`, `\??\PhysicalDriveN`,
інтерфейс `GUID_DEVINTERFACE_DISK` і запис `Enum\USBSTOR\…`; для розділу —
`\Device\HarddiskVolumeK`, `\??\Volume{…}`, `\??\HarddiskNPartitionM`.
Таблиця розділів читається й пишеться тут (MBR, GPT). Пристрій має
`unix_path`, тож читання й запис ідуть напряму через fd, повз wineserver;
IOCTL лишаються IRP до mountmgr. Такий файл має окремий тип `FD_TYPE_DISK`:
`NUL`, мейлслоти й пристрій каналів лишаються `FD_TYPE_DEVICE`. `NUL` має
`unix_path`, але не має драйвера, тож запит до драйвера для нього закінчується
помилкою (Wine 0117).

**fmifs.dll.** `FormatEx`, `Chkdsk`, `EnableVolumeCompression`,
`QueryAvailableFileSystemFormat`: FAT12/16/32, exFAT, NTFS, пише через сирий
том. Форматери перевіряються на файлах-образах (`tests/tools/format-test.c`):
`fsck.vfat`, `fsck.exfat`, `ntfsck`, `ntfsfix`, запис файлів драйвером ядра.
`Chkdsk` лише звітує про успіх: перевіряти свіжий том немає чого.

**UAC.** Відкрити диск чи том для читання або запису даних може лише підвищений
процес, інакше `ACCESS_DENIED`, як у Windows (`device_open_file`,
`token_is_limited`). Програма з `requireAdministrator` або `highestAvailable`
у маніфесті запитує підтвердження на старті: `ntdll` (`ask_consent`) звертається
до служби Appinfo, вона показує `consent.exe`. На «Ні» процес завершується з
`ERROR_CANCELLED`. Раніше Wine підвищував такі програми мовчки.

**Обмеження.** Немає VDS, UDF і ReFS у `FormatEx`, Windows To Go. Внутрішні
диски сирим записом недоступні навіть із підвищенням. `\\.\E:` тому з літерою
досі відкривається через `dosdevices/e::` прямо на вузол хоста, повз mountmgr:
перевірки підвищення там немає, а `IOCTL_STORAGE_GET_DEVICE_NUMBER` відповідає
заглушка cdrom.

## Програми на Electron перестали запускатися (9.11.2026)

Після встановлення 0116 Antigravity IDE (і будь-яка програма на Electron)
закривалася одразу після запуску, без вікна, повідомлення чи дампа; Chrome
працював. 0116 вважав диском усе з `FD_TYPE_DEVICE`, тож
`NtQueryVolumeInformationFile` для `NUL` пішов до драйвера, якого немає:
`STATUS_FILE_DELETED`, а `GetFileType` дав `FILE_TYPE_UNKNOWN`. Node для
програми без консолі відкриває `nul` замість відсутніх стандартних дескрипторів
і за такої помилки робить `_exit(134)`. Перевірка: `tests/tools/nultest.c`.

## Що знайшла VM (9.11.2026)

- Rufus 4.15 падав на старті (NULL з `GetExecutableVersion`), поки не було
  `System32\bcdboot.exe` з версією.
- `GetDriveType("\\?\Volume{…}\")` давав 0: корінь тому відкривається на
  пристрої тому, і `FileFsDeviceInformation` каже `FILE_DEVICE_DISK`, а
  kernelbase знав лише `FILE_DEVICE_DISK_FILE_SYSTEM`. Rufus через це не
  знаходив новий розділ («Waiting for logical drive to reappear»).
- Кнопки «Почати» і «Закрити» не натискалися мишею: панель із чотирма
  значками (`CCS_NOPARENTALIGN` без `CCS_NORESIZE`) comctl32 розтягував на
  всю ширину вікна (ReactOS 0110).
- Непідвищене відкриття сирого тому клало wineserver: у гілці «доступ
  заборонено» `device_file_destroy` бачив неініціалізований `file->fd`.
- Rufus просить FAT32 з кластером 64 КБ на 2 ГБ: після сканування ISO
  `SetFSFromISO` міняє ФС, поки списки ще вимкнені, і лишається список
  кластерів FAT16. Це менше 65525 кластерів, а Syslinux (як і прошивки)
  визначає тип FAT за їх кількістю й такий том бере за FAT16. `format_fat`
  зменшує кластер, поки FAT32 не стане справжнім (на 2 ГБ — 16 КБ), а
  FAT16 так само збільшує завеликий.
- Таблиця перечитується (з відмонтуванням томів диска) лише тоді, коли
  змінилися самі розділи: тип, початок, розмір, заголовок GPT, а не
  завантажувальний код чи прапорець «активний». Так і при закритті дескриптора,
  що писав диск, і на `IOCTL_DISK_UPDATE_PROPERTIES`: partmgr Windows лишає
  незмінені розділи з їхніми томами. Rufus після запису MBR просить оновлення,
  а потім створює `ldlinux.sys` через літеру тому.
- Літера, яку забрали (`DeleteVolumeMountPoint`) і дали знову, лишалась без
  посилання `dosdevices/e:`: `add_dos_device` пропускав його, бо том мав ту
  саму точку монтування. `dir E:\` казав «Файл не знайдено» при правильній
  мітці. Тепер посилання ставиться завжди, атомарно (`rename`), і не
  чіпається, коли вже вказує куди треба.
- Перевірка: `tests/tools/voldiag.c` (що бачить програма про кожен том) і
  `tests/tools/wndiag.c` (дочірні вікна і що лежить під кожною кнопкою).

**Блокування.** `FSCTL_LOCK_VOLUME` знімає файлову систему лише тоді, коли
на томі ніщо не тримає відкритих файлів, інакше `ACCESS_DENIED`, як у
Windows. `FSCTL_DISMOUNT_VOLUME` знімає її примусово. Це важливо для
USB-дисків, на яких працюють (проєкт, WSL): вони теж «керовані».
`FormatEx`, як `format` з примусовим відмонтуванням, при відмові блокування
спершу відмонтовує том.

## Літери дисків (Wine 0119)

Як у менеджері монтування Windows. База — `HKLM\SYSTEM\MountedDevices`,
зберігається між стартами: одне значення `\DosDevices\X:` на літеру, у ньому
том-власник у форматі Windows:

- розділ GPT — `DMIO:ID:` і GUID розділу;
- розділ MBR — підпис диска і зсув розділу (12 байтів);
- USB-флешка — `_??_USBSTOR#Disk&Ven_…#серійник&0#{53f56307-…}`, ім'я пристрою.
  Від запису на неї не залежить: Rufus щоразу пише новий підпис MBR, а флешка
  лишається тією самою. Так само в реєстрі Windows 10 на цьому ПК.

Том отримує літеру, яка йому належить, якщо вона вільна, інакше першу вільну
від D:. Флешка, коли її витягли, літери не тримає, тому літери не ростуть.
Літера, задана програмою (`SetVolumeMountPoint`), лишається за флешкою. У тому
одна літера: другий `IOCTL_MOUNTMGR_CREATE_POINT` отримує
`STATUS_INVALID_PARAMETER`. Розділ типу, який знає Windows (FAT, IFS, basic
data), має літеру ще без файлової системи (`rawletter=1` у записі тому).

**Навмисна відмінність від Windows.** Літера незнімного диска (внутрішнього чи
USB-SSD) лишається за ним, поки його немає, і ніхто її не займе. Флешка, що
з'явилася раніше за диски, колись забрала D: і зсунула всі літери.

`System\Arctic\DriveLetters` більше не читається: з нього один раз беруться
літери розділів GPT. Ключ лишається для відкату на попереднє оновлення.

**WSL.** `arctic-lxss` прив'язує кожну літеру в дистрибутив, і такий bind тримає
том: ядро не перечитає таблицю розділів, а витягнута флешка не звільнить ім'я
`sdd`. Тому `arctic-disk` (`dismount`), `arctic-volume` (зникнення пристрою) і
`arctic-init` (безпечне вилучення) знімають і ці прив'язки (`lxss-binds.h`), а
`host_device` не вважає їх використанням диска хостом.

На диск одразу (`RegFlushKey`) пишеться лише літера, що лишається: незнімного
диска або задана програмою. Автоматична літера флешки куща не переписує.

Неформатований том через `\\?\Volume{…}` відповідає
`STATUS_UNRECOGNIZED_VOLUME`, як RAW у Windows. Через літеру (`E:\`) запит іде
повз mountmgr на вузол хоста (див. «Обмеження») і каже `NTFS`.

Перевірка у VM (`tools/wsl/make-letter-test-disks.sh`, `--tcg`):
`tests/vm/l1-letters.txt` (флешки, неформатована, USB-диск, якого немає),
`l3-rufus-one-letter.txt` (Rufus міняє підпис MBR, літера та сама й одна),
`l4-a`, `l4-b` (диск як після встановлення, `--pull`: літера USB-диска лишається
після обриву живлення). На флешці (`--usb`) обрив живлення цього не перевіряє:
C: там пишеться у фоні, і сеанс зникає цілком.

## Перевірка

`tools/wsl/make-rufus-test-disks.sh`, `tests/vm/r*.txt`: віртуальна флешка
у VM, справжні диски не чіпаємо. `r3-rufus-write.txt` проходить увесь шлях
мишею: UAC, ISO, «Почати», ISO-режим, підтвердження. Потім записаний образ
`rufus-stick.img` завантажується окремою QEMU.

9.11.2026: Rufus 4.15 записав Alpine 3.24 (FAT32, MBR, Syslinux) за 30 с,
«Готово». Флешка під SeaBIOS: `SYSLINUX 6.04` → Alpine до `login:`.
