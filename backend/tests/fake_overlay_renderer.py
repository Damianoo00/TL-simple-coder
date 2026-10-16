"""Deterministic BGRA producer for the browser transport and compositing tests."""
import socket
import struct
import sys
import time

options = dict(argument[10:].split("=", 1) for argument in sys.argv[1:])
width, height = int(options["width"]), int(options["height"])
image = bytearray(width * height * 4)
image[4:8] = bytes((0, 0, 128, 128))  # premultiplied half-transparent red
image[8:12] = bytes((0, 255, 0, 255))
with socket.socket(socket.AF_UNIX) as connection:
    connection.connect(options["socket"])
    connection.sendall(struct.pack("<4sIIIQ", b"OVL1", width, height, len(image), 1) + image)
    # A static overlay does not paint repeatedly. Keep the transport alive.
    while True:
        time.sleep(1)
