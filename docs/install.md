# Arctic, встановлений поряд із Windows

Флешка навіть із кешем у пам'яті лишалась повільною (див. persistence.md,
«Повільна флешка»), тож 4.10.2026 Arctic поставлено на внутрішній SSD поряд
із Windows 10. Влаштування те саме, що на флешці, лише GPT і чужий розділ EFI.

## Що де лежить (ноутбук розробника)

| Що | Де |
|---|---|
| C: Arctic | диск 1 (Crucial MX500, той самий, що D:), розділ 3, NTFS «ARCTIC», 100 ГБ, GPT GUID `a1e10416-7f60-4865-a3a8-6356457cba7a`; у Windows — F: |
| D: | той самий диск, розділ 2, стиснутий на 100 ГБ (831 ГБ); в Arctic теж видно, з літерою |
| файли завантаження | `\EFI\Arctic` на розділі EFI Windows (диск 0): `shimx64.efi`, `grubx64.efi` (Limine), `mmx64.efi`, `limine.conf`, `vmlinuz`, `initrd.img`, `ARCTIC.cer` |
| пункт меню | запис прошивки «ARCTIC» → `\EFI\Arctic\shimx64.efi`, наприкінці списку: типово вантажиться Windows, Arctic — з меню F11 |

Limine шукає конфіг поряд із собою (`\EFI\Arctic\limine.conf`), тож у корені
розділу EFI нічого нового. Конфіг (`image/limine-disk.conf`) називає C:
GPT GUID-ом розділу; hash конфігу вписаний у Limine, тому набір збирається
під конкретний розділ.

## Як ставили

1. `tools/windows/install-1-partition.ps1` (адміністратор): D: стискається на
   100 ГБ, у місці — порожній GPT-розділ; його GUID — у `install-1.json`.
2. `tools/wsl/make-install-set.sh <GUID> <тека>` — набір `\EFI\Arctic`
   (підписаний ключем розробника локально, релізним у CI).
3. `tools/windows/install-2-write.ps1` (адміністратор): C: з
   `out/arctic-usb.img` (його другий розділ) на новий розділ — перший сектор
   останнім, щоб Windows не змонтувала напівзаписаний том; `\EFI\Arctic` на
   розділ EFI; `bcdedit /copy {bootmgr} /d ARCTIC` + `path
   \EFI\Arctic\shimx64.efi` + `{fwbootmgr} displayorder /addlast` — так Windows
   сама створює запис прошивки; копія пам'яті Claude у
   `C:\Users\User\.claude\projects\e--WORK-YT-project-arctic\memory`.

На першому старті C: розширюється на весь розділ (`firststart`, ntfsresize;
розділ GPT уже потрібного розміру).

## Що змінилося в системі

- initrd розуміє PARTUUID розділу GPT (GUID із таблиці розділів), не лише
  MBR флешки.
- Кеш у пам'яті й `WINE_LAZY_FLUSH` — лише коли C: на USB (`ARCTIC_C=stick`);
  на внутрішньому диску Arctic пише як звичайна система (`ARCTIC_C=disk`).
- arctic-volume ховає не весь диск, на якому C:, а лише розділ C: (весь диск —
  тільки коли використовується сам диск, як ISO, записаний на флешку): D:
  поряд лишається з літерою.

## Перевірка у VM

`tools/wsl/make-install-test-disk.sh <GUID> <набір> <образ>` робить GPT-диск
як після встановлення (EFI з `\EFI\Arctic` і копією в `\EFI\BOOT` для
прошивки VM, C: з тим самим GUID, розділ DATA), `vm-test.py --boot-disk
<образ> --uefi` вантажиться з нього: C: знайдено за GUID, «internal disk»,
розширення, DATA (D:) у «Цей ПК».

## Ще не зроблено

- Оновлення встановленої системи: «bcdboot» (`write_esp`) уміє лише MBR
  флешки; на GPT воно поки не пише `\EFI\Arctic`.
- Встановлювач з інтерфейсом: поки — ці скрипти.
