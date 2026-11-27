# SPDX-License-Identifier: LGPL-2.1-or-later
# ntos: the executive of NT in the kernel, what wineserver was (docs/ntos.md); always built in
obj-y += ntos.o
ntos-y := main.o crt.o handlers.o ex/win32k.o 	afd/sock.o cd/console.o cm/registry.o dbgk/debugger.o dxg/d3dkmt.o ex/unicode.o ex/user.o ex/winstation.o io/async.o io/change.o io/device.o io/fd.o io/file.o io/mailslot.o io/named_pipe.o io/serial.o ke/completion.o ke/event.o ke/inproc_sync.o ke/mutex.o ke/semaphore.o ke/timer.o lpc/alpc.o mm/mapping.o ob/directory.o ob/handle.o ob/object.o ob/symlink.o ps/process.o ps/procfs.o ps/ptrace.o ps/thread.o se/token.o 

# __NO_FORTIFY: the server's structures end in name[1] (Wine's variable-size
# idiom), which the kernel's field-spanning checks take for an overflow
ccflags-y := -include $(src)/shim/ntos_names.h -I$(src) -I$(src)/shim -I$(if $(NTOS_WINE_INCLUDE),$(NTOS_WINE_INCLUDE),$(src)/wine-include) -D__WINESRC__ -D__NO_FORTIFY -Wframe-larger-than=1024 -Wno-undef -Wno-missing-prototypes 	-Wno-unused-function -Wno-missing-declarations -Wno-strict-prototypes
