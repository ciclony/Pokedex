"""Sends dex.bin to the board's microSD over USB (no card reader needed).

Usage: python tools/send_dex.py COM3 [dex.bin]
"""
import os, sys, time
import serial


def main():
    port = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else "dex.bin"
    size = os.path.getsize(path)
    ser = serial.Serial(port, 921600, timeout=10)
    time.sleep(2)
    ser.reset_input_buffer()
    ser.write(f"PUT dex.bin {size}\n".encode())
    while (line := ser.readline().decode(errors="replace").strip()) != "OK":
        if not line or line == "ERR":
            sys.exit(f"board refused the upload ({line or 'no answer'})")
    t0, sent = time.time(), 0
    with open(path, "rb") as f:
        while chunk := f.read(2048):
            ser.write(chunk)
            if ser.readline().strip() != b"#":  # ack per block
                sys.exit(f"\nupload failed at {sent} bytes")
            sent += len(chunk)
            print(f"\r{sent * 100 // size}%  {sent / 1024 / (time.time() - t0):.0f} KB/s", end="", flush=True)
    print("\n" + ser.readline().decode(errors="replace").strip())


if __name__ == "__main__":
    main()
