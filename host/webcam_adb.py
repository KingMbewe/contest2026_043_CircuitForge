"""Webcam -> emulator bridge over adb (VelaPaw live camera demo).

The emulator's goldfish camera is broken on Windows and the guest virtio-net
won't route (ENETUNREACH). adb's qemud-pipe transport works, though, and lands
on the guest loopback - so we tunnel frames through it:

  device  : TCP server on guest 127.0.0.1:8081 (camera_net.c)
  this app : `adb forward tcp:<hostport> tcp:8081`, then connect to
             127.0.0.1:<hostport> and push webcam frames on request.

Usage (start AFTER the emulator has booted and `velapaw` is running):

    cd host && .venv/Scripts/python.exe webcam_adb.py

The device (server) sends 'G'; we reply with 128x128x3 RGB888 (49152 bytes).
"""

import argparse
import socket
import subprocess
import sys
import time

W = H = 128
FRAME_BYTES = W * H * 3
ADB_DEFAULT = r"C:\Users\VICTUS\AppData\Local\Android\Sdk\platform-tools\adb.exe"


def adb(adb_path, *args):
    return subprocess.run([adb_path, *args], capture_output=True, text=True)


def setup_adb(adb_path, serial, hostport, guestport):
    # Ensure the emulator transport is connected, then forward host->guest:8081.
    adb(adb_path, "connect", "localhost:5555")
    r = adb(adb_path, "-s", serial, "forward",
            f"tcp:{hostport}", f"tcp:{guestport}")
    if r.returncode != 0:
        # fall back to default device selection if the serial isn't found
        r = adb(adb_path, "forward", f"tcp:{hostport}", f"tcp:{guestport}")
    print(f"[webcam_adb] adb forward tcp:{hostport} -> tcp:{guestport}: "
          f"{'ok' if r.returncode == 0 else r.stderr.strip()}")
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adb", default=ADB_DEFAULT, help="path to adb.exe")
    ap.add_argument("--serial", default="emulator-5554")
    ap.add_argument("--hostport", type=int, default=9999,
                    help="host port (avoid 8081 - Windows reserved)")
    ap.add_argument("--guestport", type=int, default=8081)
    ap.add_argument("--cam", type=int, default=0, help="webcam index")
    args = ap.parse_args()

    import cv2
    import numpy as np

    cap = cv2.VideoCapture(args.cam)
    if not cap.isOpened():
        sys.exit(f"cannot open webcam index {args.cam}")

    setup_adb(args.adb, args.serial, args.hostport, args.guestport)
    print(f"[webcam_adb] pushing {W}x{H} RGB888 via 127.0.0.1:{args.hostport}")

    while True:
        try:
            s = socket.create_connection(("127.0.0.1", args.hostport), timeout=3)
        except OSError:
            # device server not up yet (run `velapaw` first) - retry + re-forward
            time.sleep(1.0)
            setup_adb(args.adb, args.serial, args.hostport, args.guestport)
            continue

        print("[webcam_adb] connected to device bridge")
        try:
            while True:
                req = s.recv(1)
                if not req:
                    break
                ok, frame = cap.read()
                if not ok:
                    continue
                rgb = cv2.cvtColor(cv2.resize(frame, (W, H)), cv2.COLOR_BGR2RGB)
                s.sendall(np.ascontiguousarray(rgb, dtype=np.uint8).tobytes())
        except OSError as e:
            print(f"[webcam_adb] disconnected ({e}); reconnecting")
        finally:
            s.close()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
