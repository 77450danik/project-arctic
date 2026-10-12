# Windows Hypervisor Platform (WinHvPlatform.dll) на KVM

Мета: будь-яка Windows-програма, що запускає віртуальні машини через
Windows Hypervisor Platform (QEMU `-accel whpx`, емулятор Android,
VirtualBox і VMware Workstation у режимі Hyper-V), працює в Arctic без
змін і з апаратним прискоренням — як у Windows з увімкненою «Платформою
віртуальних машин». Програми не патчимо і не налаштовуємо під Arctic:
змінюється тільки система.

## Де

| Що | Де |
|---|---|
| API (усі експорти Windows; що ще не зроблено — `E_NOTIMPL`) | `runtime/wine/modules/dlls/winhvplatform/main.c`, `.spec` |
| KVM: партиції, пам'ять, vCPU, регістри, виходи | `runtime/wine/modules/dlls/winhvplatform/unix.c` |
| заголовки WHP (з mingw-w64, public domain) | `runtime/wine/modules/include/winhvplatform*.h` |
| `/dev/kvm` | mode 0666 (udev), модуль `kvm_intel`/`kvm_amd` вантажить udev за CPU |

Wine вантажить вбудовану DLL лише коли в `C:\Windows\System32` є її файл
(поза першим налаштуванням префікса) — оновлення його кладе.

## Як відповідає Windows

- Партиція = KVM VM; пам'ять гостя = слоти KVM на сторінки самої програми
  (`WHvMapGpaRange`: адреса процесу — вона ж адреса для KVM); часткове
  зняття відображення розрізає слот.
- vCPU = KVM vCPU. `WHvDeleteVirtualProcessor` лише «паркує» його: KVM не
  вміє ні знищити vCPU, ні створити той самий номер знову, а QEMU 11
  створює vCPU 0, видаляє і створює знову.
- APIC у гіпервізорі (`LocalApicEmulation`) = split irqchip KVM: LAPIC у
  ядрі (таймер, TSC deadline, INIT/SIPI, HLT), PIC/IOAPIC — програми.
  `WHvRequestInterrupt` → `KVM_SIGNAL_MSI`; рівневі переривання мають
  MSI-маршрут на піні резервного IOAPIC, тож KVM дає `KVM_EXIT_IOAPIC_EOI`
  → `WHvRunVpExitReasonX64ApicEoi`. Стан APIC (`...InterruptControllerState2`)
  = сторінка регістрів = `KVM_GET/SET_LAPIC`.
  Режим без APIC у гіпервізорі теж є (KVM без irqchip), але сучасний QEMU
  з KVM його вже не підтримує, і доставка переривань там ненадійна.
- **Інструкцію завжди виконує KVM**, програма лише робить доступ до свого
  пристрою. Це головне: KVM на виході MMIO/PIO/MSR уже розібрав і частково
  виконав інструкцію (при MMIO-записі RIP уже далі; у рядкових інструкціях
  RSI/RDI/RCX бувають «просунуті» на елемент, що чекає), тож віддати
  інструкцію програмі на повторну емуляцію не можна. Тому:
  - PIO: кожен елемент — простий вихід IN/OUT (порт, розмір, RAX); відповідь
    — RAX програми; RIP, який програма зсуває, ігнорується. Рядковий IN
    пачкою до 1 КБ: програмі дається `rep ins` з RCX = пачка, вона пише в
    пам'ять гостя за ES:RDI (вміст збережено перед і повернено після), а
    дані звідти йдуть у KVM.
  - MMIO: кожен шматок (1/2/4 байти) — `MOV [EDI], EAX` / `MOV EAX, [EDI]`
    у показаному програмі пласкому контексті без сторінок (EDI = GPA). Вище
    4 ГБ адреса в 32 біти не влазить, тому там контекст — **довгий режим**
    (`CS.L`, `EFER.LMA`, сторінки вимкнені, CR3 ні до чого), а адреса — RDI;
    байти інструкції ті самі. Без цього пристрої, чий BAR прошивка кладе у
    вікно над пам'яттю (xHCI на машині з 4 ГБ), читалися як `0xffffffff`:
    гість казав «xHCI HW not ready ... status = 0xffffffff» і лишався без
    USB-клавіатури й миші (2026-10-12).
  - MSR (`KVM_CAP_X86_USER_SPACE_MSR` + фільтр на запис APIC base):
    відповідь — RDX:RAX; якщо програма лишила RIP на інструкції — #GP.
- XSAVE-стан (`WHvGet/SetVirtualProcessorXsaveState`, `...State` типу
  XsaveState) — у стиснутому форматі XSAVEC, як дає Hyper-V; KVM тримає
  стандартний — перекладається за CPUID 0xD.
- CPUID — таблиця KVM для гостя з кількістю процесорів, APIC ID, бітом
  гіпервізора; виходи CPUID не даються (KVM їх не має).
- `WHvCancelRunVirtualProcessor` — `immediate_exit` і сигнал потоку в KVM_RUN.

## Перевірено

2026-10-10, ноутбук (i7 Kaby Lake): звичайний QEMU 11.1 для Windows
(`qemu-system-x86_64.exe -accel whpx -smp 2`) завантажив Alpine з ISO у
вікні GTK. До того без DLL: «Could not load library WinHvPlatform.dll», TCG.

2026-10-12: з MMIO вище 4 ГБ сам Arctic вантажиться у VM під своїм же
гіпервізором — `tools/vm-test.py` з флешки: «NT ready» за 17 с, робочий стіл
за 26-33 с, USB-клавіатура й миша тестів працюють. До виправлення VM
доводилося ганяти на емуляції процесора (`vm-test.py --tcg`), у 5-10 разів
повільніше.

## Далі

- Брудні сторінки, WinHvEmulation.dll (емулятор Android).
- Швидкість: `KVM_CAP_SYNC_REGS` замість трьох ioctl на вихід.
