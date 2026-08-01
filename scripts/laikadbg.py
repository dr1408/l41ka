#!/usr/bin/env python3
"""l41ka client control

requires pyusb; --cdc also requires pyserial: python3 -m pip install pyusb pyserial

you are not really intended to use this script at all. it's primarily for hacking on the project

if you insist (i will not support this script):
```
python3 scripts/laikadbg.py --cdc & # get debug spam
python3 scripts/laikadbg.py probe (until you get dfu device)
python3 scripts/laikadbg.py exploit
python3 scripts/laikadbg.py probe (until you get pwneddfu device)
python3 scripts/laikadbg.py setup-iboot
python3 scripts/laikadbg.py probe (until you get laikadfu device)
python3 scripts/laikadbg.py send-pongo
```
Copyright (c) 0cyn All Rights Reserved
"""

from __future__ import annotations

import argparse
import json
import math
import string
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

try:
    import usb.core
    import usb.util
except ImportError:
    usb = None

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None


VID = 0x4141
PID = 0x4C41
CONTROL_INTERFACE = 2
ENDPOINT_OUT = 0x03
ENDPOINT_IN = 0x83
MAGIC = 0x4E5943414B31344C
VERSION = 1
TYPE_REQUEST = 1
TYPE_RESPONSE = 2
HEADER = struct.Struct("<QBBHIIIII")
MAX_PAYLOAD = 4096
UPLOAD_DATA_MAX = MAX_PAYLOAD - 4

PI_CHECK_ERRORS = 0xFF000001
PI_GET_INFO = 0xFF000002
PI_GET_OPEN_CONNECTION = 0xFF000003
PI_POLL_FOR_DEVICE = 0xFF000004
PI_RESET_PI = 0xFF000005
PI_ENTER_BOOTSEL = 0xFF0000FF
DEVICE_DFU_INFO = 0x01000001
DEVICE_DFU_EXPLOIT = 0x01000002
DEVICE_DFU_RAW_EP0 = 0x01000003
DEVICE_PWNED_DFU_PHYSREAD = 0x02000001
DEVICE_PWNED_DFU_PHYSWRITE = 0x02000002
DEVICE_PWNED_DFU_EXECUTE = 0x02000003
DEVICE_PWNED_DFU_SEND_IBOOT_PATCHFINDER = 0x03000001
DEVICE_PWNED_DFU_TRIGGER_IBOOT_PATCHFINDER = 0x03000002
DEVICE_PWNED_DFU_SEND_EMBEDDED_IBOOT_PATCHFINDER_AND_BOOT = 0x03000003
DEVICE_RECOVERY_REBOOT = 0x04000001
DEVICE_RECOVERY_INFO = 0x04000002
DEVICE_LAIKADFU_SEND_PAYLOAD = 0x05000001
DEVICE_LAIKADFU_TRIGGER_PAYLOAD = 0x05000002
DEVICE_LAIKADFU_SEND_EMBEDDED_PONGOOS = 0x05000003
DEVICE_PONGO_READ_OUTPUT = 0x06000001
DEVICE_PONGO_SEND_COMMAND = 0x06000002
DEVICE_PONGO_SEND_MODULE = 0x06000003
DEVICE_PONGO_TRIGGER_MODULE = 0x06000004
DEVICE_PONGO_BEGIN_UPLOAD_FILE = 0x06000005
DEVICE_PONGO_FINISH_UPLOAD_FILE = 0x06000006
DEVICE_PONGO_SEND_AND_TRIGGER_EMBEDDED_KPF = 0x060000F1
DEVICE_PONGO_SEND_AND_TRIGGER_EMBEDDED_RAMDISK = 0x060000F2
GENERIC_UPLOAD_CHUNK = 0xF1000001

STATUS_NAMES = {
    0x01000001: "invalid magic",
    0x01000002: "unsupported version",
    0x01000003: "invalid packet type",
    0x01000004: "reserved field is nonzero",
    0x01000005: "invalid request ID",
    0x01000006: "invalid request status",
    0x01000007: "payload too large",
    0x01000008: "payload CRC mismatch",
    0x01000009: "unknown opcode",
    0x0100000A: "invalid payload",
    0x0100000B: "pipelined request",
    0x02000001: "no connection",
    0x02000002: "wrong device type",
    0x02000003: "command busy",
    0x03000001: "upload not active",
    0x03000002: "upload already active",
    0x03000003: "wrong upload offset",
    0x03000004: "upload exceeds declared length",
    0x03000005: "upload incomplete",
    0x03000006: "upload CRC mismatch",
}

DEVICE_TYPES = {
    0x00000001: "dfu",
    0x00000002: "pwned-dfu",
    0x00000004: "recovery",
    0x00000005: "laika-dfu",
    0x00000006: "pongo",
    0xFFFFFFFF: "none",
}

RAW_TOKEN_PIDS = {"out": 0xE1, "setup": 0x2D}
RAW_DATA_PIDS = {"data0": 0xC3, "data1": 0x4B}
RAW_HANDSHAKES = {0x00: "timeout", 0xD2: "ack", 0x5A: "nak", 0x1E: "stall"}


class ProtocolError(RuntimeError):
    pass


class CdcDebugStream:
    def __init__(self, serial_number: str | None):
        if serial is None:
            raise RuntimeError("PySerial is required; install it with: python3 -m pip install pyserial")

        port = None
        deadline = time.monotonic() + 3.0
        while port is None:
            candidates = [
                candidate for candidate in serial.tools.list_ports.comports()
                if candidate.vid == VID and candidate.pid == PID
            ]
            matching = [candidate for candidate in candidates if candidate.serial_number == serial_number]
            if matching:
                port = matching[0]
            elif serial_number is None and len(candidates) == 1:
                port = candidates[0]
            elif time.monotonic() >= deadline:
                raise RuntimeError("no matching l41ka CDC debug port found")
            else:
                time.sleep(0.1)

        self._port = serial.Serial(port.device, timeout=0.1)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._drain, name="l41ka-cdc-debug", daemon=True)
        self._thread.start()

    def _drain(self) -> None:
        while not self._stop.is_set():
            try:
                data = self._port.read(self._port.in_waiting or 1)
            except serial.SerialException as error:
                if not self._stop.is_set():
                    print(f"laikadbg: CDC debug stream failed: {error}", file=sys.stderr)
                return
            if data:
                sys.stderr.buffer.write(data)
                sys.stderr.buffer.flush()

    def close(self) -> None:
        self._stop.set()
        self._thread.join()
        self._port.close()

    def wait(self) -> None:
        self._thread.join()

    def __enter__(self) -> CdcDebugStream:
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()


class LaikaClient:
    def __init__(self, serial_number: str | None = None):
        if usb is None:
            raise RuntimeError("PyUSB is required; install it with: python3 -m pip install pyusb")
        devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID) or [])
        if serial_number is not None:
            devices = [device for device in devices if usb.util.get_string(device, device.iSerialNumber) == serial_number]
        if not devices:
            raise RuntimeError(f"no l41ka USB device found ({VID:04x}:{PID:04x})")
        if len(devices) != 1:
            serials = [usb.util.get_string(device, device.iSerialNumber) or "unknown" for device in devices]
            raise RuntimeError(f"multiple l41ka devices found ({', '.join(serials)}); use --serial")
        self._device = devices[0]
        self._detached = False
        try:
            self._device.set_configuration()
        except usb.core.USBError as error:
            if not self._device.get_active_configuration():
                raise error
        if hasattr(self._device, "is_kernel_driver_active"):
            try:
                if self._device.is_kernel_driver_active(CONTROL_INTERFACE):
                    self._device.detach_kernel_driver(CONTROL_INTERFACE)
                    self._detached = True
            except (NotImplementedError, usb.core.USBError):
                pass
        usb.util.claim_interface(self._device, CONTROL_INTERFACE)
        self._next_id = 1
        self._receive_buffer = bytearray()

    def close(self) -> None:
        usb.util.release_interface(self._device, CONTROL_INTERFACE)
        if self._detached:
            try:
                self._device.attach_kernel_driver(CONTROL_INTERFACE)
            except (NotImplementedError, usb.core.USBError):
                pass
        usb.util.dispose_resources(self._device)

    def __enter__(self) -> LaikaClient:
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def _read_exact(self, length: int, timeout_ms: int) -> bytes:
        deadline = time.monotonic() + timeout_ms / 1000.0
        while len(self._receive_buffer) < length:
            remaining_ms = max(1, math.ceil((deadline - time.monotonic()) * 1000))
            if time.monotonic() >= deadline:
                raise TimeoutError("timed out waiting for USB response")
            # Always provide room for a full endpoint packet; a 32-byte header may
            # share its 64-byte USB packet with the start of the response payload.
            chunk = self._device.read(ENDPOINT_IN, 64, timeout=remaining_ms)
            self._receive_buffer.extend(chunk)
        result = bytes(self._receive_buffer[:length])
        del self._receive_buffer[:length]
        return result

    def request(self, opcode: int, payload: bytes = b"", timeout_ms: int = 70000) -> bytes:
        if not 0 <= opcode <= 0xFFFFFFFF:
            raise ValueError("opcode must fit uint32")
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f"payload exceeds {MAX_PAYLOAD} bytes")
        request_id = self._next_id
        self._next_id = 1 if request_id == 0xFFFFFFFF else request_id + 1
        header = HEADER.pack(
            MAGIC, VERSION, TYPE_REQUEST, 0, opcode, request_id, len(payload), 0, zlib.crc32(payload) & 0xFFFFFFFF
        )
        packet = header + payload
        written = self._device.write(ENDPOINT_OUT, packet, timeout=timeout_ms)
        if written != len(packet):
            raise RuntimeError(f"short USB write: {written} of {len(packet)} bytes")

        response_header = self._read_exact(HEADER.size, timeout_ms)
        magic, version, kind, reserved, response_opcode, response_id, length, status, crc = HEADER.unpack(response_header)
        if magic != MAGIC or version != VERSION or kind != TYPE_RESPONSE or reserved != 0:
            raise ProtocolError("malformed response header")
        if response_opcode != opcode or response_id != request_id:
            raise ProtocolError(
                f"mismatched response: opcode={response_opcode:#x}, id={response_id}; expected {opcode:#x}, {request_id}"
            )
        if length > MAX_PAYLOAD:
            raise ProtocolError(f"response payload exceeds {MAX_PAYLOAD} bytes")
        response = self._read_exact(length, timeout_ms) if length else b""
        if zlib.crc32(response) & 0xFFFFFFFF != crc:
            raise ProtocolError("response payload CRC mismatch")
        if status:
            raise ProtocolError(f"{STATUS_NAMES.get(status, 'unknown status')} ({status:#010x})")
        return response


def c_string(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def print_json(value: object) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def connection_from_payload(payload: bytes) -> dict[str, object]:
    if len(payload) != 8:
        raise ProtocolError("connection response has wrong length")
    opened, device_type = struct.unpack("<II", payload)
    return {"open": bool(opened), "device_type": DEVICE_TYPES.get(device_type, f"unknown-{device_type:#x}")}


def decode_device_info(payload: bytes) -> dict[str, object]:
    if len(payload) != 576:
        raise ProtocolError("device-info response has wrong length")
    present, cpid, cprv, cpfm, scep, bdid, ibfl = struct.unpack_from("<7I", payload, 0)
    ecid = struct.unpack_from("<Q", payload, 32)[0]
    pid, interface_number, pwned, ap_length, sep_length = struct.unpack_from("<5I", payload, 40)
    return {
        "present_fields": f"0x{present:08x}",
        "cpid": f"0x{cpid:04x}",
        "cprv": f"0x{cprv:02x}",
        "cpfm": f"0x{cpfm:02x}",
        "scep": f"0x{scep:02x}",
        "bdid": f"0x{bdid:02x}",
        "ibfl": f"0x{ibfl:02x}",
        "ecid": f"0x{ecid:016x}",
        "pid": f"0x{pid:04x}",
        "interface": interface_number,
        "pwned": bool(pwned),
        "srtg": c_string(payload[60:124]),
        "srnm": c_string(payload[124:156]),
        "imei": c_string(payload[156:188]),
        "ap_nonce": payload[188 : 188 + min(ap_length, 64)].hex(),
        "sep_nonce": payload[252 : 252 + min(sep_length, 64)].hex(),
        "serial": c_string(payload[316:444]),
        "product_type": c_string(payload[444:476]),
        "hardware_model": c_string(payload[476:508]),
        "display_name": c_string(payload[508:572]),
    }


def file_crc32(path: Path) -> tuple[int, int]:
    length = path.stat().st_size
    crc = 0
    with path.open("rb") as source:
        while chunk := source.read(64 * 1024):
            crc = zlib.crc32(chunk, crc)
    return length, crc & 0xFFFFFFFF


def stream_upload(client: LaikaClient, path: Path, begin: int, trigger: int, minimum: int, maximum: int,
                  timeout_ms: int) -> None:
    length, crc = file_crc32(path)
    if not minimum <= length <= maximum:
        raise ValueError(f"upload must contain {minimum}-{maximum} bytes")
    try:
        client.request(begin, struct.pack("<II", length, crc), timeout_ms)
    except (OSError, RuntimeError, TimeoutError) as error:
        raise RuntimeError(f"upload begin failed: {error}") from error
    chunk_size = UPLOAD_DATA_MAX
    if begin == DEVICE_LAIKADFU_SEND_PAYLOAD:
        chunk_size -= chunk_size % 64
    offset = 0
    with path.open("rb") as source:
        while chunk := source.read(chunk_size):
            try:
                response = client.request(GENERIC_UPLOAD_CHUNK, struct.pack("<I", offset) + chunk, timeout_ms)
            except (OSError, RuntimeError, TimeoutError) as error:
                raise RuntimeError(f"upload chunk at offset {offset} failed: {error}") from error
            if len(response) != 4:
                raise ProtocolError("upload chunk response has wrong length")
            next_offset = struct.unpack("<I", response)[0]
            offset += len(chunk)
            if next_offset != offset:
                raise ProtocolError(f"device acknowledged offset {next_offset}, expected {offset}")
    try:
        client.request(trigger, timeout_ms=timeout_ms)
    except (OSError, RuntimeError, TimeoutError) as error:
        raise RuntimeError(f"upload trigger failed: {error}") from error


def drain_pongo_output(client: LaikaClient, timeout_ms: int) -> None:
    while True:
        payload = client.request(DEVICE_PONGO_READ_OUTPUT, timeout_ms=timeout_ms)
        if len(payload) < 12:
            raise ProtocolError("Pongo output response is too short")
        length, more, dropped = struct.unpack_from("<III", payload)
        data = payload[12:]
        if length != len(data):
            raise ProtocolError("Pongo output length mismatch")
        if dropped:
            print(f"laikadbg: warning: {dropped} bytes of Pongo output were overwritten", file=sys.stderr)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        if not more:
            return


def print_hex_dump(address: int, data: bytes) -> None:
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        hexadecimal = " ".join(f"{byte:02x}" for byte in chunk)
        text = "".join(chr(byte) if byte >= 0x20 and chr(byte) in string.printable else "." for byte in chunk)
        print(f"{address + offset:016x}: {hexadecimal:<47}  {text}")


def read_memory(client: LaikaClient, address: int, length: int, timeout_ms: int) -> bytes:
    output = bytearray()
    while len(output) < length:
        chunk_length = min(MAX_PAYLOAD, length - len(output))
        payload = struct.pack("<QII", address + len(output), chunk_length, 0)
        chunk = client.request(DEVICE_PWNED_DFU_PHYSREAD, payload, timeout_ms)
        if len(chunk) != chunk_length:
            raise ProtocolError("physical read returned the wrong length")
        output.extend(chunk)
    return bytes(output)


def write_memory(client: LaikaClient, address: int, data: bytes, timeout_ms: int) -> None:
    offset = 0
    while offset < len(data):
        chunk = data[offset : offset + MAX_PAYLOAD - 8]
        client.request(DEVICE_PWNED_DFU_PHYSWRITE, struct.pack("<Q", address + offset) + chunk, timeout_ms)
        offset += len(chunk)


def raw_ep0_request(client: LaikaClient, data: bytes, token: str, pid: str,
                    count: int, delay_us: int, timeout_ms: int) -> list[str]:
    payload = struct.pack(
        "<BBHIII", RAW_TOKEN_PIDS[token], RAW_DATA_PIDS[pid], len(data), count, delay_us, 0
    ) + data
    response = client.request(DEVICE_DFU_RAW_EP0, payload, timeout_ms=timeout_ms)
    if len(response) != count:
        raise ProtocolError(f"raw EP0 response has {len(response)} handshakes, expected {count}")
    return [RAW_HANDSHAKES.get(value, f"unknown-{value:#04x}") for value in response]


def unsigned_integer(value: str) -> int:
    parsed = int(value, 0)
    if not 0 <= parsed <= 0xFFFFFFFFFFFFFFFF:
        raise argparse.ArgumentTypeError("integer must fit uint64")
    return parsed


def hex_data(value: str) -> bytes:
    if value.startswith("@"):
        return Path(value[1:]).read_bytes()
    return bytes.fromhex(value.removeprefix("0x").replace(" ", "").replace(":", "").replace("_", ""))


def run_command(client: LaikaClient, args: argparse.Namespace, timeout_ms: int) -> None:
    command = args.command
    if command == "errors":
        payload = client.request(PI_CHECK_ERRORS, timeout_ms=timeout_ms)
        if len(payload) != 32:
            raise ProtocolError("error record has wrong length")
        fields = struct.unpack("<8I", payload)
        print_json(dict(zip(("present", "reason", "request_id", "opcode", "context", "value", "uptime_ms", "reserved"), fields)))
    elif command == "info":
        payload = client.request(PI_GET_INFO, timeout_ms=timeout_ms)
        if len(payload) != 128:
            raise ProtocolError("Pi info has wrong length")
        print_json({"board": c_string(payload[:64]), "firmware_version": c_string(payload[64:])})
    elif command == "connection":
        print_json(connection_from_payload(client.request(PI_GET_OPEN_CONNECTION, timeout_ms=timeout_ms)))
    elif command == "probe":
        print_json(connection_from_payload(client.request(PI_POLL_FOR_DEVICE, timeout_ms=timeout_ms)))
    elif command == "reset":
        client.request(PI_RESET_PI, timeout_ms=timeout_ms)
    elif command == "bootsel":
        client.request(PI_ENTER_BOOTSEL, timeout_ms=timeout_ms)
    elif command in ("dfu-info", "recovery-info"):
        opcode = DEVICE_DFU_INFO if command == "dfu-info" else DEVICE_RECOVERY_INFO
        print_json(decode_device_info(client.request(opcode, timeout_ms=timeout_ms)))
    elif command == "exploit":
        client.request(DEVICE_DFU_EXPLOIT, timeout_ms=timeout_ms)
    elif command == "dfu-raw":
        data = hex_data(args.data) if args.data else b""
        names = raw_ep0_request(client, data, args.token, args.pid, args.count, args.delay_us, timeout_ms)
        counts = {name: names.count(name) for name in sorted(set(names))}
        print_json({"handshakes": names, "counts": counts})
    elif command == "read":
        data = read_memory(client, args.address, args.length, timeout_ms)
        if args.output:
            Path(args.output).write_bytes(data)
        else:
            print_hex_dump(args.address, data)
    elif command == "write":
        write_memory(client, args.address, hex_data(args.data), timeout_ms)
    elif command == "exec":
        arguments = args.args + [0] * (8 - len(args.args))
        payload = struct.pack("<QII8Q", args.address, 1 if args.el1 else 0, len(args.args), *arguments)
        response = client.request(DEVICE_PWNED_DFU_EXECUTE, payload, timeout_ms)
        if len(response) < 68:
            raise ProtocolError("execute response is too short")
        registers = struct.unpack_from("<8Q", response)
        body_length = struct.unpack_from("<I", response, 64)[0]
        if len(response) != 68 + body_length:
            raise ProtocolError("execute response body length mismatch")
        print_json({"registers": [f"0x{value:x}" for value in registers], "body": response[68:].hex()})
    elif command == "setup-iboot":
        client.request(DEVICE_PWNED_DFU_SEND_EMBEDDED_IBOOT_PATCHFINDER_AND_BOOT, timeout_ms=timeout_ms)
    elif command == "iboot-patchfinder":
        stream_upload(client, args.path, DEVICE_PWNED_DFU_SEND_IBOOT_PATCHFINDER,
                      DEVICE_PWNED_DFU_TRIGGER_IBOOT_PATCHFINDER, 104, 16383, timeout_ms)
    elif command == "reboot":
        client.request(DEVICE_RECOVERY_REBOOT, timeout_ms=timeout_ms)
    elif command == "load-pongo":
        client.request(DEVICE_LAIKADFU_SEND_EMBEDDED_PONGOOS, timeout_ms=timeout_ms)
    elif command == "load-kpf":
        client.request(DEVICE_PONGO_SEND_AND_TRIGGER_EMBEDDED_KPF, timeout_ms=timeout_ms)
        drain_pongo_output(client, timeout_ms)
    elif command == "load-ramdisk":
        client.request(DEVICE_PONGO_SEND_AND_TRIGGER_EMBEDDED_RAMDISK, timeout_ms=timeout_ms)
        drain_pongo_output(client, timeout_ms)
    elif command == "laika-image":
        stream_upload(client, args.path, DEVICE_LAIKADFU_SEND_PAYLOAD,
                      DEVICE_LAIKADFU_TRIGGER_PAYLOAD, 1, 0x200000, timeout_ms)
    elif command == "pongo-command":
        text = " ".join(args.value).encode("utf-8")
        client.request(DEVICE_PONGO_SEND_COMMAND, struct.pack("<I", min(timeout_ms, 60000)) + text, timeout_ms)
        if text not in (b"reset", b"reset\n"):
            drain_pongo_output(client, timeout_ms)
    elif command == "pongo-module":
        stream_upload(client, args.path, DEVICE_PONGO_SEND_MODULE,
                      DEVICE_PONGO_TRIGGER_MODULE, 1, 128 * 1024 * 1024, timeout_ms)
        drain_pongo_output(client, timeout_ms)
    elif command == "pongo-file":
        stream_upload(client, args.path, DEVICE_PONGO_BEGIN_UPLOAD_FILE,
                      DEVICE_PONGO_FINISH_UPLOAD_FILE, 1, 128 * 1024 * 1024, timeout_ms)
    elif command == "request":
        print(client.request(args.opcode, hex_data(args.payload) if args.payload else b"", timeout_ms).hex())
    else:
        raise ValueError(f"unsupported command: {command}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--serial", help="RP2350 USB serial number when multiple controllers are attached")
    parser.add_argument("--timeout", type=float, default=70.0, help="USB request timeout in seconds")
    parser.add_argument("--cdc", action="store_true", help="continuously print CDC debug output")
    commands = parser.add_subparsers(dest="command")
    for name in ("errors", "info", "connection", "probe", "reset", "bootsel", "dfu-info", "recovery-info", "exploit",
                 "setup-iboot", "reboot", "load-pongo", "load-kpf", "load-ramdisk"):
        commands.add_parser(name)
    read = commands.add_parser("read")
    read.add_argument("address", type=unsigned_integer)
    read.add_argument("length", type=unsigned_integer)
    read.add_argument("-o", "--output")
    write = commands.add_parser("write")
    write.add_argument("address", type=unsigned_integer)
    write.add_argument("data", help="hex bytes, or @file")
    execute = commands.add_parser("exec")
    execute.add_argument("address", type=unsigned_integer)
    execute.add_argument("args", nargs="*", type=unsigned_integer)
    execute.add_argument("--el1", action="store_true")
    raw_ep0 = commands.add_parser("dfu-raw")
    raw_ep0.add_argument("data", nargs="?", default="", help="0-64 payload bytes as hex, or @file")
    raw_ep0.add_argument("--token", choices=RAW_TOKEN_PIDS, default="setup")
    raw_ep0.add_argument("--pid", choices=RAW_DATA_PIDS, default="data0")
    raw_ep0.add_argument("--count", type=unsigned_integer, default=1)
    raw_ep0.add_argument("--delay-us", type=unsigned_integer, default=0)
    iboot = commands.add_parser("iboot-patchfinder")
    iboot.add_argument("path", type=Path)
    laika = commands.add_parser("laika-image")
    laika.add_argument("path", type=Path)
    pongo = commands.add_parser("pongo-command")
    pongo.add_argument("value", nargs="+")
    module = commands.add_parser("pongo-module")
    module.add_argument("path", type=Path)
    pongo_file = commands.add_parser("pongo-file")
    pongo_file.add_argument("path", type=Path)
    raw = commands.add_parser("request")
    raw.add_argument("opcode", type=unsigned_integer)
    raw.add_argument("payload", nargs="?", help="hex payload or @file")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.cdc == (args.command is not None):
        parser.error("specify either --cdc or a command")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a positive finite number")
    if args.command == "read" and args.length == 0:
        parser.error("read length must be positive")
    if args.command == "exec" and len(args.args) > 8:
        parser.error("exec accepts at most eight arguments")
    if args.command == "dfu-raw":
        data = hex_data(args.data) if args.data else b""
        if len(data) > 64:
            parser.error("dfu-raw payload cannot exceed 64 bytes")
        if not 1 <= args.count <= MAX_PAYLOAD:
            parser.error(f"dfu-raw --count must be between 1 and {MAX_PAYLOAD}")
        if args.delay_us > 1_000_000:
            parser.error("dfu-raw --delay-us cannot exceed 1000000")
    timeout_ms = min(0x7FFFFFFF, math.ceil(args.timeout * 1000))
    try:
        if args.cdc:
            with CdcDebugStream(args.serial) as debug:
                debug.wait()
            return 0
        with LaikaClient(args.serial) as client:
            run_command(client, args, timeout_ms)
    except (OSError, RuntimeError, ValueError, TimeoutError) as error:
        print(f"laikadbg: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("laikadbg: interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
