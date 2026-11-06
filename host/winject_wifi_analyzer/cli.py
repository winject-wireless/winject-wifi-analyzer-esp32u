from __future__ import annotations

import argparse
import asyncio
import logging
from pathlib import Path
import socket
import time

from .mplane import MPlaneClient
from .protocol import FLAG_LAST_CAPTURE, StreamAssembler
from .radio import DATA_PORT_DEFAULT


def capture(args: argparse.Namespace) -> int:
    """Grabs --count captures and writes each as complex64 .cf32 (|x|=1 = 0 dBFS)."""
    control = MPlaneClient(args.host, args.mplane_port, args.timeout)
    control.request(f"iq_set freq={args.frequency} gain={args.gain}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as data:
        data.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        data.bind((args.bind, args.data_port))
        data.settimeout(args.timeout)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((args.host, 9))
            local = probe.getsockname()[0]
        control.request(f"iq_capture n={args.samples} rate={args.rate} bits={args.bits} "
                        f"count={args.count} dst={local}:{data.getsockname()[1]}")
        radio = socket.gethostbyname(args.host)
        assembler = StreamAssembler()
        done = 0
        deadline = time.monotonic() + args.timeout + args.count * 0.05
        out = Path(args.output)
        while done < args.count and time.monotonic() < deadline:
            datagram, peer = data.recvfrom(2048)
            if peer[0] != radio:
                continue
            cap = assembler.add(datagram)
            if cap is None:
                continue
            path = out if args.count == 1 else out.with_name(f"{out.stem}_{done:04d}{out.suffix}")
            path.write_bytes(cap.samples().tobytes())
            print(f"{path}: capture={cap.capture_id} n={cap.sample_count} bits={cap.bits} "
                  f"fs={cap.sample_rate_hz} f={cap.frequency_hz} gain={cap.gain_index}")
            done += 1
            if cap.flags & FLAG_LAST_CAPTURE:
                break
    if done < args.count:
        raise TimeoutError(f"got {done} of {args.count} captures")
    return 0


def serve(args: argparse.Namespace) -> int:
    from .server import serve as run

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    asyncio.run(run(args.host, args.http_host, args.http_port, args.data_port))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="winject-wifi-analyzer")
    parser.add_argument("--host", required=True, help="ESP32U address")
    parser.add_argument("--mplane-port", type=int, default=22)
    parser.add_argument("--timeout", type=float, default=3.0)
    sub = parser.add_subparsers(dest="command", required=True)
    for name, command in (("info", "iq_info"), ("ping", "ping"), ("stop", "iq_stop")):
        item = sub.add_parser(name)
        item.set_defaults(wire_command=command)
    item = sub.add_parser("send", help="send one raw mplane line")
    item.add_argument("line", nargs="+")
    item = sub.add_parser("capture")
    item.add_argument("--frequency", type=int, default=2437)
    item.add_argument("--gain", default="agc", help="agc or 0..gain_max")
    item.add_argument("--samples", type=int, default=16380)
    item.add_argument("--rate", type=int, choices=(80000000, 40000000, 16000000),
                      default=16000000)
    item.add_argument("--bits", type=int, choices=(8, 10), default=8)
    item.add_argument("--count", type=int, default=1)
    item.add_argument("--bind", default="0.0.0.0")
    item.add_argument("--data-port", type=int, default=DATA_PORT_DEFAULT)
    item.add_argument("--output", required=True, help=".cf32 output path")
    item.set_defaults(run=capture)
    item = sub.add_parser("serve", help="run the web app")
    item.add_argument("--http-host", default="0.0.0.0")
    item.add_argument("--http-port", type=int, default=8080)
    item.add_argument("--data-port", type=int, default=DATA_PORT_DEFAULT)
    item.set_defaults(run=serve)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if hasattr(args, "run"):
        return args.run(args)
    client = MPlaneClient(args.host, args.mplane_port, args.timeout)
    line = " ".join(args.line) if args.command == "send" else args.wire_command
    print(client.request(line))
    return 0
