# Boot screen art

`segoe_slboot_ex.ttf` — Segoe Boot Semilight from Windows 11 (10.0.22621.6489,
the boot font of cumulative update KB5073454; the update carries it as a
"null" delta in `n\`, which msdelta's ApplyDeltaB turns into the font).
Microsoft's. Its glyphs U+E100..U+E176 are the 119 frames of Windows 11's
boot spinner; `ci/mkspinner.py` renders them for the initrd's boot screen
(`host/init/bootanim.c`).
