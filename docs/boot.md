# Завантаження на справжньому залізі

Збірка дає два образи:

| Файл | Для чого |
|---|---|
| `arctic-usb.img` (у релізі `arctic-usb.img.xz`) | флешка: пишеться посекторно (Rufus, balenaEtcher, `xzcat \| dd`), вантажиться і з UEFI (з Secure Boot теж), і з BIOS/CSM. Зберігає все, що на неї записано: C: — розділ на ній ([persistence.md](persistence.md)) |
| `arctic.iso` | диски й віртуальні машини, нічого не зберігає |

Флешка — MBR із двома розділами:

| # | Тип | Розмір | Вміст |
|---|---|---|---|
| 1 | EFI (0xEF), активний, FAT32 `ARCTIC` | 500 МБ | `EFI\BOOT` (shim, Limine, MokManager), `EFI\Arctic` (ядро, initrd), `boot\limine`, `ARCTIC.cer` |
| 2 | NTFS (0x07), `ARCTIC` | решта | C:; Linux-хост у `C:\Windows\System32\Host` |

initrd знаходить C: за PARTUUID (підпис диска + номер розділу), який
вписаний у `limine.conf` флешки; підпис диска однаковий у всіх збірках, щоб
оновлення підходило. На першому старті C: розширюється на всю флешку. Меню
Limine приховане: клавіша, натиснута в першу секунду, показує його з пунктами
**Arctic, undo the last update** і **Arctic, nothing saved** (зміни йдуть у
пам'ять, як з ISO). Подробиці — [persistence.md](persistence.md).

## Екран завантаження (4.10.2026)

Як у Windows 11: логотип Arctic на 38,2 % висоти, під ним (на ¾ висоти)
крутилка Windows 11 — справжня, з її шрифту завантажувача
(`runtime/art/boot/segoe_slboot_ex.ttf`, гліфи U+E100…E176, 119 кадрів,
60 кадрів/с; `ci/mkspinner.py` рендерить їх для initrd у 15 розмірах).

- Малює окремий процес (`host/init/bootanim.c`), який initrd запускає першим
  ділом і який живе до робочого столу. Логотип не гасне й не з'являється
  вдруге: прошивка показує свій логотип (на ПК — виробника, у VM — TianoCore),
  initrd одним кадром замінює його на логотип Arctic, а коли GPU-драйвер
  забирає екран у simpledrm, процес за ~0,1 с малює все на новій карті.
- Windows лишає на екрані логотип виробника (ACPI BGRT); Arctic показує свій.
- Під крутилкою — те, що пише Windows: «Підготовка пристрою» на першому
  старті, «Сканування й відновлення диска (C:): виконано N%» (з прогресу
  ntfsck).
- Оновлення й відкат — екран без логотипа, крутилка вище і «Робота з
  оновленнями, виконано N%» / «Скасування змін, внесених до комп’ютера»,
  «Не вимикайте комп’ютер».
- На старті сеансу arctic-init передає екран dwm.exe: процес віддає DRM
  master, а крутилка обертається далі, поки dwm не покаже свій кадр. (У VM на
  bochs-drm вона при цьому стоїть: без DIRTYFB, який потребує master, QEMU під
  WHPX не бачить змін.)

Перевірка: `vm-test.py --film SECONDS` знімає екран кожні ~0,15 с від
увімкнення (`out/test-local/film`), `tools/wsl/make-dev-initrd.sh` +
`--append … --initrd` — новий initrd без збирання образу.

## Чому флешка не вантажилася (21.09.2026)

Флешку записали Rufus'ом у режимі «ISO»: він розпаковує файли на FAT32-розділ,
а не копіює образ посекторно. Звідси обидва симптоми:

- **UEFI** — прошивка вантажила `EFI\BOOT\BOOTX64.EFI`, Limine вантажив ядро й
  initrd, а initrd шукав носій, монтуючи блокові пристрої **лише як iso9660**.
  FAT32-розділ не підходив під цю умову: STOP `INACCESSIBLE_BOOT_DEVICE`.
- **CSM** — Rufus не вміє ставити завантажувач BIOS для образу з Limine, тому
  для прошивки диск просто не завантажувальний.

Виправлено: `is_media()` пробує iso9660, vfat, exfat, udf, ext4; з'явився
`ci/make-usb-img.sh`, який збирає `arctic-usb.img` з Limine в MBR.

У dev-збірках (`arctic.dev=1`) на екрані STOP у полі «Що спричинило збій»
стоять імена дисків, які initrd бачив, — так видно, чого бракує: драйвера
контролера чи файлової системи.

## Перевірка локально

```
python tools/vm-test.py out/arctic-usb.img --usb            # BIOS/CSM
python tools/vm-test.py out/arctic-usb.img --usb --uefi     # UEFI (OVMF)
python tools/vm-test.py --usb --fresh                       # щойно записана флешка
```

`--usb` вантажить не сам образ, а qcow2-шар над ним (`out/test-local/stick.qcow2`,
розміром `--stick-size`, типово 8G): записане в одному запуску лишається для
наступного, а зібраний образ не змінюється. Новий образ або `--fresh` —
це знову щойно записана флешка.

Локальна VM запускається без Secure Boot; з ним образ перевіряє CI (див. «Secure Boot»).

## Secure Boot (25.09.2026)

Secure Boot можна не вимикати. Раніше прошивка сама вантажила Limine, а Limine
ніхто не підписував, тож із увімкненим Secure Boot флешка не вантажилася.
Тепер ланцюжок такий:

| Файл на носії | Що це | Ким підписано / як перевіряється |
|---|---|---|
| `EFI\BOOT\BOOTX64.EFI` | shim 16.1 із Debian 13 | Microsoft UEFI CA 2011: йому довіряє кожен ПК із Secure Boot |
| `EFI\BOOT\grubx64.efi` | Limine (shim вантажить друге завантаження саме під цим ім'ям) | ключ Arctic; shim перевіряє підпис за MokList |
| `EFI\BOOT\mmx64.efi` | MokManager: екран, де ПК реєструє ключ | Debian; shim знає цей ключ |
| `ARCTIC.cer` | сертифікат Arctic, який реєструють у MokManager | — |
| `boot/limine/limine.conf` | конфіг Limine; в ISO і на флешці свій, тож і Limine підписується для кожного окремо | BLAKE2b конфігу вшито в підписаний Limine |
| `arctic/vmlinuz`, `arctic/initrd.img` (на флешці `EFI/Arctic/…`) | ядро та initrd | BLAKE2b кожного файла записано в `limine.conf` |

З вимкненим Secure Boot або в BIOS/CSM усе працює, як і раніше: shim без
Secure Boot нічого не перевіряє, а BIOS-стадія Limine та сама.

### Перший запуск на ПК з Secure Boot

Робиться один раз на кожному ПК, далі ключ пам'ятає прошивка (змінна MokList).

1. Завантажитися з флешки. Shim каже `Verification failed: (0x1A) Security Violation` — натиснути **OK**
   (Enter).
2. Відкривається синій екран **Perform MOK management**. Протягом 10 секунд натиснути будь-яку клавішу, інакше
   ПК просто перезавантажиться.
3. **Enroll key from disk** → вибрати флешку (`ARCTIC`; з диска ISO — `ARCTICEFI`) → **ARCTIC.cer**.
4. **Continue** → **Yes** → **Reboot**.
5. Після перезавантаження знову вибрати флешку: тепер Arctic вантажиться одразу.

Щоб переконатися, що це справжній ключ Arctic, на кроці 3 можна натиснути **View key**. Відбитки:

- SHA-1 (його показує MokManager): `84:02:F6:99:B7:BB:66:EF:F4:33:8F:D2:E9:6A:8F:8C:9A:DB:09:98`
- SHA-256: `58:E0:88:E4:9D:DC:26:A1:52:70:83:E6:25:A9:84:85:03:50:F6:6B:2B:C6:9D:D2:D4:74:2E:49:8D:20:92:14`

Ключ реєструється в shim, а не в базі db прошивки. Тож Windows, BitLocker і
налаштування BIOS це не зачіпає.

### Якщо не допомогло

- **Прошивка сама пише «Secure Boot Violation» / «Invalid signature», а синього
  екрана MokManager немає.** Отже, прошивка не довіряє навіть shim: у ній вимкнено
  сертифікат Microsoft для сторонніх завантажувачів. У налаштуваннях Secure Boot
  треба увімкнути **Allow Microsoft 3rd Party UEFI CA** (Lenovo, Secured-core ПК),
  на Surface — **Microsoft & 3rd party CA**. На Acer пункти Secure Boot стають
  доступні лише після того, як задати Supervisor Password.
- **Shim каже, що його відкликано (`SBAT self-check failed`, `Something has gone seriously wrong`).**
  Оновлення Windows додало в прошивку новіший рівень SBAT, ніж у нашого shim.
  Треба оновити shim (див. нижче) і перезібрати образ.

### Збірка

- Ключ підпису — RSA-2048, `CN=Project Arctic Secure Boot 2026`. Приватна частина:
  - секрет репозиторію **`ARCTIC_SB_KEY`** (Settings → Secrets and variables → Actions);
  - резервна копія поза репозиторієм, у мейнтейнера.
- Сертифікат лежить у репозиторії: `image/secureboot/arctic-sb.crt`.
  **Ключ змінювати не можна:** з новим ключем кожен ПК доведеться реєструвати ще раз.
- `ci/sign-efi.sh` (його викликає `ci/build-image.sh`) робить таке:
  1. Завантажує shim і MokManager, перевіряючи їхні SHA-256. Якщо версію вже прибрали з дзеркала Debian, бере
     файл із snapshot.debian.org за SHA-1.
  2. Додає в Limine секцію `.sbat`: у збірці Arch її немає, а без неї shim 15.3+ нічого не вантажить.
  3. Вшиває хеш `limine.conf` (`limine enroll-config`).
  4. Підписує Limine (`sbsign`) і перевіряє, що підпис відповідає сертифікату.
  5. Для ISO збирає `efiboot.img` — FAT-образ для El Torito.
  `ci/make-usb-img.sh` викликає його вдруге, для розділу EFI флешки з її власним `limine.conf`.
- Локальна збірка (WSL):
  `ARCTIC_SB_KEY_FILE=/шлях/до/arctic-sb.key bash tools/wsl/build-image.sh`.
  Без ключа образ підписується тимчасовим ключем розробника з `out/secureboot-dev/`. Такий образ теж
  вантажиться з Secure Boot, але зареєструвати доведеться його `ARCTIC.cer`, а не релізний.
  CI без секрету образ не збирає.
- `limine.conf` у репозиторії лишається без хешів: їх дописує збірка. Після
  `enroll-config` конфіг на носії змінювати не можна, бо з Secure Boot Limine
  тоді зупиниться з помилкою. Змінювати треба `image/limine.conf` і перезбирати образ.
- Оновити shim: у `SHIM_DEBS` у `ci/sign-efi.sh` вписати нові пакети
  `shim-signed` та `shim-helpers-amd64-signed` з Debian, їхні SHA-256 і SHA-1.
  Підпис має лишатися Microsoft UEFI CA 2011, поки ПК з лише сертифікатом 2023 не стануть звичайними.

### Що перевіряє CI (`ci/smoke-test.sh`)

| Тест | Прошивка | Носій | Очікування |
|---|---|---|---|
| `uefi` | OVMF без Secure Boot | ISO | NT піднявся (ПК без Secure Boot) |
| `uefi-sb` | OVMF, Secure Boot, ключі Microsoft, ключ Arctic у MokList | ISO | NT піднявся |
| `usb-sb` | те саме | `arctic-usb.img` як USB-флешка | NT піднявся |
| `sb-denied` | OVMF, Secure Boot, ключа Arctic немає | `arctic-usb.img` | ядро **не** стартувало |

### Чого поки немає

Secure Boot тут потрібен для того, щоб Arctic вантажився, не вимикаючи його. Захистом
від чужого коду в системі він поки не є:

- `host.sqfs` і C: не перевіряються (далі — dm-verity);
- ядро не вмикає lockdown, тож модулі без підпису (nvidia) вантажаться.
