# M1 — перші пікселі: робочі нотатки

Що знайдено у вихідниках Wine 11.18 і як це лягає на план (розділ 3.4–3.5 у PLAN.md).
Документ робочий: оновлюється, поки йде M1.

## Звідки береться драйвер дисплея

- `explorer.exe /desktop` читає `HKCU\Software\Wine\Drivers\Graphics` (за замовчуванням
  `mac,x11,wayland`) і вантажить перший, що запрацює, як `wine<name>.drv`
  (`programs/explorer/desktop.c`, `load_graphics_driver`).
- Ім'я вибраного драйвера публікується в `HKLM\...\Control\Video\{GUID}\0000\GraphicsDriver`,
  і всі інші процеси беруть його звідти (`dlls/win32u/driver.c`).
- **У нас:** `Graphics=arctic` → `winearctic.drv`. Жодного X11 чи Wayland-драйвера в образі.

## Хто володіє робочим столом

- У Wine вікном робочого столу володіє `explorer.exe /desktop`: він вантажить драйвер і обробляє
  `DesktopWindowProc`. Режим `root` (`DF_WINE_ROOT_DESKTOP`) означає «робочий стіл — це весь екран».
- У Windows робочий стіл належить win32k/csrss, а explorer — лише оболонка.
- **У нас:** роль власника робочого столу переходить у `csrss.exe` (root-desktop на весь віртуальний
  екран). `explorer.exe` стає чистою оболонкою — ReactOS explorer без жодних Wine-обов'язків.

## winearctic.drv = winewayland.drv мінус керування вікнами

`dlls/winewayland.drv` (~8 тис. рядків) робить кожне вікно верхнього рівня `xdg_toplevel`, дочірні
GL/Vulkan-поверхні — `wl_subsurface`, пристрої дисплея бере з `wl_output`, ввід — з `wl_seat`.

| Частина winewayland | У winearctic.drv |
|---|---|
| `window_surface.c` (DIB → shm-буфер) | лишається |
| `wayland_surface.c` (xdg_toplevel, configure/ack) | роль `arctic_surface_v1`: поверхня ↔ HWND, без configure-переговорів |
| `window.c`: `is_window_managed`, managed-логіка | прибрати: жодне вікно не «managed», рамки/переміщення робить user32 (як віртуальний робочий стіл Wine) |
| `vulkan.c`, `opengl.c` | лишаються (VK_KHR_wayland_surface / EGL на внутрішньому з'єднанні) |
| `display.c`, `wayland_output.c` | `arctic_display_v1`: GPU, конектори, EDID, режими від dwm.exe |
| `wayland_keyboard.c`, `wayland_pointer.c`, `wayland_text_input.c` | прибрати: ввід іде від csrss.exe напряму у wineserver |
| `wayland_data_device.c` (буфер обміну) | прибрати: буфер обміну — суто Windows-механізм wineserver |
| xdg-output, xdg-decoration, cursor-shape, pointer-constraints/warp, fractional-scale, text-input, data-control | прибрати |

## Геометрія і z-order: джерело — wineserver

- Спільна пам'ять wineserver (`server/protocol.def`, `session_shm_t`) уже має `desktop_shm_t`
  (курсор, стан клавіш, лічильник моніторів), `input_shm_t` (active, focus, capture, move_size),
  `window_shm_t` (DPI, клас, extra). **Прямокутників вікон і z-order там немає.**
- **Патч у wineserver:** лічильник `zorder_serial` у `desktop_shm_t` (росте при будь-якій зміні
  позиції, розміру, видимості чи порядку вікна верхнього рівня) + запит, що повертає список вікон
  верхнього рівня в z-order з їхніми видимими прямокутниками та регіонами.
- `dwm.exe` на кожному кадрі дивиться на `zorder_serial` і перечитує список лише при зміні.
  Клієнт надсилає тільки буфер + серійний номер конфігурації вікна, для якої він намальований.
- Позиція курсора вже є в `desktop_shm_t.cursor` — dwm рухає апаратний курсор без IPC.

## Ввід: csrss.exe

- Вприскування — `NtUserSendHardwareInput` (`include/ntuser.h`), апаратний, без прапорця INJECTED.
- Хіт-тест, фокус, захоплення робить wineserver, як win32k у Windows.

## Де живе наш код Wine

Форк репозиторію Wine на GitHub був би публічним: форки публічних репозиторіїв не бувають приватними.
Тому все лежить у нашому приватному репозиторії, за схемою, як у wine-staging:

- `runtime/wine/modules/` — нові модулі цілком (`dlls/winearctic.drv`, `programs/dwm`,
  `programs/csrss`, …), копіюються в дерево upstream перед збіркою;
- `runtime/wine/patches/` — невеликі нумеровані патчі до наявних файлів (wineserver, win32u,
  реєстрація модулів у `configure.ac`);
- `ci/build-wine.sh` бере тег upstream, накладає модулі й патчі, запускає `tools/make_makefiles`
  та `autoreconf` і збирає. Ребейз на новий upstream = зміна тегу + правка патчів, що не наклались.

## Порядок робіт у M1

1. Патч wineserver: `zorder_serial` + запит списку вікон.
2. `dwm.exe` + `dwm.so`: KMS dumb-буфери, pixman, сервер буферів (wl_compositor, wl_shm,
   wl_subcompositor + `arctic_surface_v1`, `arctic_display_v1`).
3. `winearctic.drv`: форк winewayland.drv за таблицею вище.
4. `csrss.exe`: власник root-робочого столу + RIT на libinput.
5. `wininit.exe` / `winlogon.exe` / `userinit.exe` і порядок старту з arctic-init.
6. Реєстр за замовчуванням: `Graphics=arctic`, uk-UA, розкладки, Mizu.
   MUI-завантаження ресурсів у рантаймі (`<тека модуля>\uk-UA\<модуль>.mui`) і винесення перекладів
   Wine та ReactOS у MUI-файли під час збірки.
7. Оболонка з ReactOS: explorer + shell32/browseui/shdocvw. Сумісність перевіряємо на самому початку
   й виправляємо; explorer Wine не використовуємо.
