from __future__ import annotations

import argparse
from pathlib import Path
import socket
import time

from .mplane import MPlaneClient
from .protocol import CaptureAssembler


def capture(args: argparse.Namespace) -> int:
    control = MPlaneClient(args.host, args.mplane_port, args.timeout)
    gain = args.gain if args.gain == "hardware" else str(int(args.gain))
    control.request(f"analyzer_set freq={args.frequency} gain={gain}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as data:
        data.bind((args.bind, args.data_port))
        data.settimeout(args.timeout)
        control.request(
            f"analyzer_capture samples={args.samples} rate={args.rate} "
            f"bits={args.bits} port={data.getsockname()[1]}"
        )
        expected_host = socket.gethostbyname(args.host)
        assembler = CaptureAssembler()
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            datagram, peer = data.recvfrom(2048)
            if peer[0] != expected_host:
                continue
            result = assembler.add(datagram)
            if result is not None:
                Path(args.output).write_bytes(result.payload)
                print(
                    f"capture={result.capture_id} samples={result.sample_count} "
                    f"bits={result.bits} bytes={len(result.payload)}"
                )
                return 0
    raise TimeoutError("capture did not complete")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--mplane-port", type=int, default=22)
    parser.add_argument("--timeout", type=float, default=3.0)
    sub = parser.add_subparsers(dest="command", required=True)
    for name, command in (("info", "analyzer_info"), ("ping", "ping")):
        item = sub.add_parser(name)
        item.set_defaults(wire_command=command)
    item = sub.add_parser("capture")
    item.add_argument("--frequency", type=int, default=2442)
    item.add_argument("--gain", default="hardware")
    item.add_argument("--samples", type=int, default=16380)
    item.add_argument("--rate", type=int, choices=(80000000, 40000000, 16000000), default=16000000)
    item.add_argument("--bits", type=int, choices=(8, 10), default=8)
    item.add_argument("--bind", default="0.0.0.0")
    item.add_argument("--data-port", type=int, default=9001)
    item.add_argument("--output", required=True)
    item.set_defaults(run=capture)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if hasattr(args, "run"):
        return args.run(args)
    print(MPlaneClient(args.host, args.mplane_port, args.timeout).request(args.wire_command))
    return 0
