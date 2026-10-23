"""Saves what the board's screen shows as a PNG, optionally tapping first.

Usage: python tools/screenshot.py COM3 out.png [x,y ...]
Each x,y is a tap (screen pixels), done in order with a pause before the capture.
"""
import sys, time
import serial
from PIL import Image


def main():
    port, out, taps = sys.argv[1], sys.argv[2], sys.argv[3:]
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 921600, 5
    ser.dtr = ser.rts = False  # opening the port must not reset the board
    ser.open()
    time.sleep(0.3)
    for t in taps:
        ser.write(f"TAP {t.replace(',', ' ')}\n".encode())
        time.sleep(1.2)  # the home grid reads 6 cards from the SD
    ser.reset_input_buffer()
    ser.write(b"SHOT\n")
    while not (line := ser.readline().decode(errors="replace")).startswith("SHOT"):
        if not line:
            sys.exit("no answer from the board")
    w, h = map(int, line.split()[1:])
    raw = ser.read(w * h * 2)
    if len(raw) != w * h * 2:
        sys.exit(f"short screenshot ({len(raw)} bytes)")
    img = Image.new("RGB", (w, h))
    px = [(((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
          for c in (raw[i] | raw[i + 1] << 8 for i in range(0, len(raw), 2))]
    img.putdata(px)
    img.save(out)
    print(out)


if __name__ == "__main__":
    main()
