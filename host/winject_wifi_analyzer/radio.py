"""Async link to the ESP32U IQ server: mplane control (UDP 22) and WJIQ v2
sample datagrams (UDP 5300)."""
from __future__ import annotations

import asyncio
import socket
from typing import Callable

from .protocol import Capture, StreamAssembler

DATA_PORT_DEFAULT = 5300
MULTICAST_GROUP_DEFAULT = "239.255.53.1"


class MPlaneError(RuntimeError):
    pass


def parse_kv(line: str) -> dict[str, str]:
    """`name k=v k=v` -> {k: v} (the command name is under "_")."""
    parts = line.split()
    out = {"_": parts[0] if parts else ""}
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            out[k] = v
    return out


class _Control(asyncio.DatagramProtocol):
    def __init__(self, link: "RadioLink") -> None:
        self.link = link

    def datagram_received(self, data: bytes, addr) -> None:
        self.link._on_reply(data.decode("utf-8", "replace"))


class _Data(asyncio.DatagramProtocol):
    def __init__(self, link: "RadioLink") -> None:
        self.link = link

    def datagram_received(self, data: bytes, addr) -> None:
        if addr[0] != self.link.radio_ip:
            return
        self.link.datagrams += 1
        self.link.bytes += len(data)
        cap = self.link.assembler.add(data)
        if cap is not None and self.link.on_capture is not None:
            self.link.on_capture(cap)


class RadioLink:
    def __init__(self, host: str, mplane_port: int = 22,
                 data_port: int = DATA_PORT_DEFAULT,
                 on_capture: Callable[[Capture], None] | None = None,
                 multicast_group: str = MULTICAST_GROUP_DEFAULT) -> None:
        self.radio_ip = socket.gethostbyname(host)
        self.mplane_port = mplane_port
        self.data_port = data_port
        self.multicast_group = multicast_group
        self.on_capture = on_capture
        self.assembler = StreamAssembler()
        self.datagrams = 0
        self.bytes = 0
        self._ctl: asyncio.DatagramTransport | None = None
        self._pending: dict[int, asyncio.Future] = {}
        self._next_id = 0
        self._lock = asyncio.Lock()
        self.local_ip = self._local_ip_towards(self.radio_ip)

    @staticmethod
    def _local_ip_towards(ip: str) -> str:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((ip, 9))
            return s.getsockname()[0]

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        self._ctl, _ = await loop.create_datagram_endpoint(
            lambda: _Control(self), remote_addr=(self.radio_ip, self.mplane_port))
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", self.data_port))
        membership = socket.inet_aton(self.multicast_group) + socket.inet_aton(self.local_ip)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
        await loop.create_datagram_endpoint(lambda: _Data(self), sock=sock)

    def _on_reply(self, text: str) -> None:
        lines = [l for l in text.strip().splitlines() if l]
        by_id: dict[int, list[str]] = {}
        for line in lines:
            head, _, rest = line.partition(" ")
            if ":" not in head:
                continue
            status, _, rid = head.partition(":")
            if not rid.isdigit():
                continue
            by_id.setdefault(int(rid), []).append(f"{status} {rest}".strip())
        for rid, got in by_id.items():
            fut = self._pending.pop(rid, None)
            if fut is not None and not fut.done():
                fut.set_result(got)

    async def request(self, command: str, timeout: float = 2.0) -> list[str]:
        """Sends one mplane line; returns its reply lines without the
        `OK:<id>` decoration. Raises MPlaneError on NOK or timeout."""
        async with self._lock:
            self._next_id = self._next_id % 255 + 1
            rid = self._next_id
            fut = asyncio.get_running_loop().create_future()
            self._pending[rid] = fut
            self._ctl.sendto(f"cmd:{rid} {command}\n".encode())
        try:
            lines = await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self._pending.pop(rid, None)
            raise MPlaneError(f"timeout: {command}") from None
        out = []
        for line in lines:
            status, _, rest = line.partition(" ")
            if status == "NOK":
                raise MPlaneError(f"{command}: NOK {rest}")
            out.append(rest)
        return out

    async def info(self) -> dict[str, str]:
        merged: dict[str, str] = {}
        for line in await self.request("iq_info"):
            merged.update(parse_kv(line))
        merged.pop("_", None)
        return merged

    @property
    def dst(self) -> str:
        return f"{self.multicast_group}:{self.data_port}"
