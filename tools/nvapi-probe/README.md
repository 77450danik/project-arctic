# NVAPI probes (Linux, libnvidia-api.so.1)

Build in the `arctic-build` container, run on the host through hamam
(the container has no /dev/nvidia*). `LIB` is libnvidia-api.so.1 of the
same version as the loaded kernel module (from the driver .run).

    gcc -O1 -o probe probe.c -ldl     # GET of every function Afterburner uses, as root and as nobody
    gcc -O1 -o probe2 probe2.c -ldl   # SET of the same value back (curve, Voltage %, limits, lock)
    gcc -O1 -o probe3 probe3.c -ldl   # SetPstates20 per domain (core / memory offset), same value
    hamam -d arctic-build -- "/mnt/d/.../probe /path/libnvidia-api.so.1"

probe2/probe3 write only what they just read; re-GET must show 0 bytes changed.
Results of 21.11.2026 (GTX 1050 Mobile, 580.178.04): docs/afterburner.md.
