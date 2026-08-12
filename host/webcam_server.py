"""Webcam -> TCP bridge for the VelaPaw emulator demo.

The emulator's goldfish camera can't access the host webcam on Windows, so this
streams the real webcam into the device instead: the device (camera_net.c)
connects via the emulator's user-net (10.0.2.2) and requests frames; this server
grabs a webcam frame, resizes to 128x128 RGB888, and sends it (49152 bytes).

Run this BEFORE launching the emulator (or it will retry):

    cd host && .venv/Scripts/python.exe webcam_server.py
    # then in another terminal: ./emulator.sh nuttx/build   (type: velapaw)

Protocol: client sends 1 byte 'G' -> server sends W*H*3 bytes RGB888.
"""

import argparse
import socket
import sys

W = H = 128
FRAME_BYTES = W * H * 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8127)
    ap.add_argument("--cam", type=int, default=0, help="webcam index")
    args = ap.parse_args()

    import cv2
    import numpy as np

    cap = cv2.VideoCapture(args.cam)
    if not cap.isOpened():
        sys.exit(f"cannot open webcam index {args.cam}")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"[webcam_server] listening on 0.0.0.0:{args.port}, "
          f"serving {W}x{H} RGB888 (guest connects to 10.0.2.2:{args.port})")

    while True:
        conn, addr = srv.accept()
        print(f"[webcam_server] client {addr} connected")
        conn.settimeout(10)
        try:
            while True:
                req = conn.recv(1)
                if not req:
                    break
                ok, frame = cap.read()
                if not ok:
                    continue
                rgb = cv2.cvtColor(cv2.resize(frame, (W, H)), cv2.COLOR_BGR2RGB)
                conn.sendall(np.ascontiguousarray(rgb, dtype=np.uint8).tobytes())
        except Exception as e:
            print(f"[webcam_server] client gone: {e}")
        finally:
            conn.close()


if __name__ == "__main__":
    main()
