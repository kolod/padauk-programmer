# SPDX-License-Identifier: GPL-3.0-or-later
"""Byte transports: USB CDC serial port (DTR asserted) and TCP (simulator)."""
from __future__ import annotations

import select
import socket
import time

USB_VID = 0x1209
USB_PID = 0x0001


class TransportError(Exception):
    pass


class Transport:
    def read(self, timeout: float) -> bytes:
        raise NotImplementedError

    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class TcpTransport(Transport):
    def __init__(self, host: str, port: int, connect_timeout: float = 5.0):
        deadline = time.monotonic() + connect_timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=1.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise TransportError(f"cannot connect to {host}:{port}")
                time.sleep(0.05)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.setblocking(False)

    def read(self, timeout: float) -> bytes:
        r, _, _ = select.select([self.sock], [], [], max(timeout, 0))
        if not r:
            return b""
        try:
            data = self.sock.recv(65536)
        except BlockingIOError:
            return b""
        except OSError as e:
            raise TransportError(str(e))
        if not data:
            raise TransportError("connection closed")
        return data

    def write(self, data: bytes) -> None:
        self.sock.setblocking(True)
        try:
            self.sock.sendall(data)
        except OSError as e:
            raise TransportError(str(e))
        finally:
            self.sock.setblocking(False)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class SerialTransport(Transport):
    def __init__(self, port: str):
        import serial  # pyserial

        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0
        self.ser.dtr = True
        self.ser.open()

    def read(self, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while True:
            n = self.ser.in_waiting
            if n:
                return self.ser.read(n)
            if time.monotonic() >= deadline:
                return b""
            time.sleep(0.001)

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def close(self) -> None:
        self.ser.close()


def find_serial_port() -> str | None:
    from serial.tools import list_ports

    for p in list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID:
            return p.device
    return None


def open_transport(spec: str | None) -> Transport:
    """`tcp://host:port`, a serial port name, or None for auto-detection by VID/PID."""
    if spec and spec.startswith("tcp://"):
        host, _, port = spec[6:].rpartition(":")
        return TcpTransport(host or "127.0.0.1", int(port))
    port = spec or find_serial_port()
    if not port:
        raise TransportError("programmer not found (USB 1209:0001)")
    return SerialTransport(port)
