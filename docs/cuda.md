# CUDA (nvcuda.dll)

17.10.2026. Програми, що рахують на картці NVIDIA через CUDA (Blender Cycles, відеокодери, ML), бачать у Arctic
справжню картку. `nvcuda.dll` — модуль Wine `runtime/wine/modules/dlls/nvcuda`, що передає кожен виклик CUDA Driver
API драйверу хоста, `libcuda.so.1` (драйвер NVIDIA 580 у `host.sqfs`).

## Як це влаштовано

- Програма й драйвер в одному процесі (Wine — процес Linux), тож контексти, модулі, вказівники пам'яті картки й
  закріплені буфери хоста, які роздає драйвер, однакові з обох боків. Ядра програм (cubin, PTX) ті самі, що на
  Linux.
- Усі функції Driver API приймають лише цілі й вказівники та повертають `CUresult`, тож їх обслуговує один
  перехідник: бік Windows (`main.c`) передає 14 аргументів як 64-бітні числа, бік Unix (`unix.c`) викликає функцію
  драйвера з ними (System V читає стільки, скільки оголошено).
- Модуль генерує `tools/gen-nvcuda.py` зі списку функцій драйвера хоста (`libcuda-580.178.04.syms`, з
  `nm -D --defined-only /usr/lib/libcuda.so.1`). Новий драйвер — новий список і перегенерація.
- Не передаються (`CUDA_ERROR_NOT_SUPPORTED`): функції з колбеком у програму (`cuStreamAddCallback`,
  `cuLaunchHostFunc`, `cuLogsRegisterCallback`; драйвер викликав би код Windows за конвенцією System V у своєму
  потоці), функції, що роздають вказівники драйвера (`cuGetProcAddress*`, `cuGetExportTable`), і дві з `float` за
  значенням (`cuTexRefSetMipmapLevelBias/Clamp`). `cuOccupancyMaxPotentialBlockSize*` передається, коли колбек
  розміру блоку NULL (Cycles передає NULL).
- Лише x86_64: 32-бітна `nvcuda.dll` нічого не експортує.

## Перевірено (GTX 1050, драйвер 580.178.04)

- `python tests/cuda/cuda_smoke.py`: драйвер CUDA 13.0, «NVIDIA GeForce GTX 1050, sm_61, 1994 MB», 64 MB на
  картку й назад без помилок.
- Blender 4.5.3 LTS: Cycles бачить «NVIDIA GeForce GTX 1050» як пристрій CUDA; кадр 1080p, 40 семплів, сцена
  кімнати трейлера Blastforce — 50 с (на процесорі i7-7700HQ — близько 17 хв).

## Чого немає

- CUDA Runtime через `cudart64_*.dll` програми працює, лише поки вона не кличе `cuGetExportTable` (кличе майже
  завжди): потрібні таблиці експорту з перехідниками. NVENC/NVDEC (`nvEncodeAPI64.dll`, `nvcuvid.dll`) і NVML —
  окремі модулі, ще не зроблені. OptiX потрібні RTX-картки.
