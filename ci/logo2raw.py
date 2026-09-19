"""Converts the boot logo PNG into the raw format host/init/splash.c reads."""
import struct
import sys

from PIL import Image

image = Image.open(sys.argv[1]).convert("RGB")
width, height = image.size
with open(sys.argv[2], "wb") as out:
    out.write(b"ARLG" + struct.pack("<II", width, height))
    out.write(image.tobytes("raw", "BGRX"))
