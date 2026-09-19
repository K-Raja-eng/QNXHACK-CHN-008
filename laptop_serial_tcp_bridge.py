#!/usr/bin/env python3
"""
Windows laptop bridge:
    Arduino USB serial -> TCP/IP -> Raspberry Pi running QNX

The Arduino sketch is intentionally unchanged. This program forwards each
line exactly as received (normalising CR/LF to LF) to the QNX TCP listener.
"""

import argparse
import socket
import time

import serial


def parse_args():
    p = argparse.ArgumentParser(description="Arduino serial to QNX TCP bridge")
    p.add_argument("--serial", required=True, help="Arduino COM port, e.g. COM5")
    p.add_argument("--pi", required=True, help="Raspberry Pi IP address")
    p.add_argument("--tcp-port", type=int, default=9100, help="QNX TCP port")
    p.add_argument("--baud", type=int, default=9600, help="Arduino serial baud")
    p.add_argument("--connect-retry", type=float, default=2.0,
                   help="Seconds between TCP reconnect attempts")
    return p.parse_args()


def connect_to_pi(host, port, retry_delay):
    while True:
        try:
            sock = socket.create_connection((host, port), timeout=5)
            sock.settimeout(None)
            print(f"[BRIDGE] TCP connected to {host}:{port}")
            return sock
        except OSError as exc:
            print(f"[BRIDGE] TCP connect failed: {exc}; retrying in {retry_delay:g}s")
            time.sleep(retry_delay)


def main():
    args = parse_args()

    print(f"[BRIDGE] Opening Arduino {args.serial} @ {args.baud} baud")
    ser = serial.Serial(args.serial, args.baud, timeout=1)

    sock = None
    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue

            # Preserve the Arduino data format; only ensure one LF-terminated
            # TCP frame per Arduino serial line.
            line = raw.decode("utf-8", errors="replace").strip("\r\n")
            payload = (line + "\n").encode("utf-8")

            while True:
                if sock is None:
                    sock = connect_to_pi(args.pi, args.tcp_port, args.connect_retry)
                try:
                    sock.sendall(payload)
                    print(f"[BRIDGE] {line}")
                    break
                except OSError as exc:
                    print(f"[BRIDGE] TCP send failed: {exc}; reconnecting")
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None

    except KeyboardInterrupt:
        print("\n[BRIDGE] Stopped by user.")
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        ser.close()


if __name__ == "__main__":
    main()
