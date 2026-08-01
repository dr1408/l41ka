#!/usr/bin/env python3
"""
manual interactor for handler.S (from host (not pi))

requires pyusb
    python3 -m pip install pyusb
"""

from __future__ import annotations

import argparse
import string
import struct
import sys
from pathlib import Path

try:
    import usb.core
except ImportError:
    usb = None


APPLE_VID = 0x05AC
DFU_PID = 0x1227

DFU_DNLOAD = 1
DFU_UPLOAD = 2
DFU_GETSTATUS = 3

REQUEST_TO_HOST_STANDARD_DEVICE = 0x80
REQUEST_TO_DEVICE_CLASS_INTERFACE = 0x21
REQUEST_TO_HOST_CLASS_INTERFACE = 0xA1
USB_REQ_GET_DESCRIPTOR = 0x06
USB_DESC_DEVICE = 0x01
USB_DESC_STRING = 0x03

CONTROL_REQUEST_VALUE = 0xFFFF
MESSAGE_MAGIC = 0x2D2A4E5741502A2D
MESSAGE_HEADER = struct.Struct("<QIIQQ")
MESSAGE_BODY_SIZE = 0x100
MESSAGE_SIZE = MESSAGE_HEADER.size + MESSAGE_BODY_SIZE

MESSAGE_READ = ord("r")
MESSAGE_WRITE = ord("w")
MESSAGE_EXECUTE = ord("x")
MESSAGE_EXECUTE_EL1 = ord("X")
MESSAGE_SET_BOOT_LR = ord("b")


class MissingPyUSBError(RuntimeError):
    pass


def unsigned_u64(value: str) -> int:
    try:
        result = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from error
    if not 0 <= result <= 0xFFFFFFFFFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in an unsigned 64-bit integer")
    return result


def positive_length(value: str) -> int:
    result = unsigned_u64(value)
    if result == 0:
        raise argparse.ArgumentTypeError("length must be greater than zero")
    return result


def parse_data(value: str) -> bytes:
    if value.startswith("@"):
        return Path(value[1:]).read_bytes()
    normalized = value.removeprefix("0x").replace(" ", "").replace(":", "").replace("_", "")
    try:
        return bytes.fromhex(normalized)
    except ValueError as error:
        raise ValueError("data must be hexadecimal bytes or @ followed by a file path") from error


def validate_range(address: int, length: int) -> None:
    if length > 0x10000000000000000 - address:
        raise ValueError("memory range exceeds the unsigned 64-bit address space")


class HandlerClient:
    def __init__(self, timeout_ms: int = 1000):
        if usb is None:
            raise MissingPyUSBError("PyUSB is required; install it with: python3 -m pip install pyusb")
        self.timeout_ms = timeout_ms
        self.device = usb.core.find(idVendor=APPLE_VID, idProduct=DFU_PID)
        if self.device is None:
            raise RuntimeError(f"no Apple DFU device found ({APPLE_VID:04x}:{DFU_PID:04x})")

        serial = self.read_serial()
        if "PWND:[" not in serial:
            raise RuntimeError(f"DFU device is not pwned (serial: {serial or '<empty>'})")

    def transfer(self, request_type: int, request: int, value: int, data_or_length: object):
        return self.device.ctrl_transfer(
            request_type, request, value, 0, data_or_length, timeout=self.timeout_ms
        )

    def read_serial(self) -> str:
        descriptor = self.transfer(
            REQUEST_TO_HOST_STANDARD_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 18
        )
        if len(descriptor) != 18:
            raise RuntimeError(f"short USB device descriptor: got {len(descriptor)} bytes")
        serial_index = descriptor[16]
        if serial_index == 0:
            return ""

        header = self.transfer(
            REQUEST_TO_HOST_STANDARD_DEVICE,
            USB_REQ_GET_DESCRIPTOR,
            (USB_DESC_STRING << 8) | serial_index,
            2,
        )
        if len(header) != 2 or header[0] < 2:
            raise RuntimeError("invalid USB serial string descriptor")
        raw = bytes(
            self.transfer(
                REQUEST_TO_HOST_STANDARD_DEVICE,
                USB_REQ_GET_DESCRIPTOR,
                (USB_DESC_STRING << 8) | serial_index,
                header[0],
            )
        )
        if len(raw) < header[0]:
            raise RuntimeError(f"short USB serial string descriptor: got {len(raw)} bytes")
        return raw[2 : header[0]].decode("utf-16le", "replace")

    def command(self, message_type: int, address: int = 0, body: bytes = b"", length: int | None = None) -> bytes:
        if length is None:
            length = len(body)
        if len(body) > MESSAGE_BODY_SIZE or length > MESSAGE_BODY_SIZE:
            raise ValueError(f"handler messages are limited to 0x{MESSAGE_BODY_SIZE:x} body bytes")

        message = bytearray(MESSAGE_SIZE)
        MESSAGE_HEADER.pack_into(message, 0, MESSAGE_MAGIC, message_type, 0, address, length)
        message[MESSAGE_HEADER.size : MESSAGE_HEADER.size + len(body)] = body

        # Walk DFU back to an idle download state before replacing its buffer.
        self.transfer(REQUEST_TO_DEVICE_CLASS_INTERFACE, DFU_DNLOAD, 0, b"\0" * 16)
        self.transfer(REQUEST_TO_DEVICE_CLASS_INTERFACE, DFU_DNLOAD, 0, None)
        self.transfer(REQUEST_TO_HOST_CLASS_INTERFACE, DFU_GETSTATUS, 0, 6)
        self.transfer(REQUEST_TO_HOST_CLASS_INTERFACE, DFU_GETSTATUS, 0, 6)
        self.transfer(REQUEST_TO_DEVICE_CLASS_INTERFACE, DFU_DNLOAD, 0, message)

        raw = bytes(
            self.transfer(
                REQUEST_TO_HOST_CLASS_INTERFACE, DFU_UPLOAD, CONTROL_REQUEST_VALUE, MESSAGE_SIZE
            )
        )
        if len(raw) != MESSAGE_SIZE:
            raise RuntimeError(f"short handler response: got 0x{len(raw):x} of 0x{MESSAGE_SIZE:x} bytes")
        magic, response_type, _reserved, response_address, response_length = MESSAGE_HEADER.unpack_from(raw)
        if magic != MESSAGE_MAGIC:
            raise RuntimeError(f"bad handler response magic: 0x{magic:016x}")
        if (response_type, response_address, response_length) != (message_type, address, length):
            raise RuntimeError("handler response header does not match the request")
        return raw[MESSAGE_HEADER.size:]

    def read_memory(self, address: int, length: int) -> bytes:
        validate_range(address, length)
        result = bytearray()
        while len(result) < length:
            chunk_length = min(MESSAGE_BODY_SIZE, length - len(result))
            body = self.command(MESSAGE_READ, address + len(result), length=chunk_length)
            result.extend(body[:chunk_length])
        return bytes(result)

    def write_memory(self, address: int, data: bytes) -> None:
        validate_range(address, len(data))
        offset = 0
        while offset < len(data):
            chunk = data[offset : offset + MESSAGE_BODY_SIZE]
            self.command(MESSAGE_WRITE, address + offset, chunk)
            offset += len(chunk)

    def execute(self, address: int, arguments: list[int], el1: bool = False) -> tuple[int, ...]:
        if len(arguments) > 8:
            raise ValueError("exec accepts at most eight arguments (x0-x7)")
        registers = arguments + [0] * (8 - len(arguments))
        body = self.command(
            MESSAGE_EXECUTE_EL1 if el1 else MESSAGE_EXECUTE,
            address,
            struct.pack("<8Q", *registers),
        )
        return struct.unpack_from("<8Q", body)

    def set_boot_lr(self, address: int) -> bool:
        body = self.command(MESSAGE_SET_BOOT_LR, address, b"\0" * 8)
        return struct.unpack_from("<Q", body)[0] == 1


def print_hexdump(data: bytes, address: int) -> None:
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        hexadecimal = " ".join(f"{byte:02x}" for byte in chunk)
        text = "".join(chr(byte) if byte >= 0x20 and chr(byte) in string.printable else "." for byte in chunk)
        print(f"{address + offset:016x}: {hexadecimal:<47}  {text}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--timeout-ms", type=positive_length, default=1000, help="USB transfer timeout")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("serial", help="print the DFU serial number")

    read = commands.add_parser("read", help="read device memory")
    read.add_argument("address", type=unsigned_u64)
    read.add_argument("length", type=positive_length)
    read.add_argument("-o", "--output", type=Path)

    write = commands.add_parser("write", help="write device memory")
    write.add_argument("address", type=unsigned_u64)
    write.add_argument("data", help="hexadecimal bytes, or @file")

    execute = commands.add_parser("exec", help="call a function and print returned x0-x7")
    execute.add_argument("address", type=unsigned_u64)
    execute.add_argument("arguments", nargs="*", type=unsigned_u64)
    execute.add_argument("--el1", action="store_true", help="enter EL1 for the call")

    boot_lr = commands.add_parser("set-boot-lr", help="replace and sign the main task return address")
    boot_lr.add_argument("address", type=unsigned_u64)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    client = HandlerClient(args.timeout_ms)

    if args.command == "serial":
        print(client.read_serial())
    elif args.command == "read":
        data = client.read_memory(args.address, args.length)
        if args.output is not None:
            args.output.write_bytes(data)
            print(f"read 0x{len(data):x} bytes to {args.output}")
        else:
            print_hexdump(data, args.address)
    elif args.command == "write":
        data = parse_data(args.data)
        if not data:
            raise ValueError("write data cannot be empty")
        client.write_memory(args.address, data)
        print(f"wrote 0x{len(data):x} bytes")
    elif args.command == "exec":
        registers = client.execute(args.address, args.arguments, args.el1)
        for index, value in enumerate(registers):
            print(f"x{index} = 0x{value:016x}")
    elif args.command == "set-boot-lr":
        if not client.set_boot_lr(args.address):
            raise RuntimeError("handler rejected the boot return address")
        print(f"set boot LR to 0x{args.address:016x}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MissingPyUSBError, RuntimeError, ValueError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
    except Exception as error:
        if usb is not None and isinstance(error, usb.core.USBError):
            print(f"USB error: {error}", file=sys.stderr)
            raise SystemExit(1)
        raise
