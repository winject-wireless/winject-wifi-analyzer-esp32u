from __future__ import annotations

import socket


class MPlaneError(RuntimeError):
    pass


class MPlaneClient:
    """Blocking mplane client: `cmd:<id> <line>` -> `OK:<id> ...` / `NOK:<id> ...`."""

    def __init__(self, host: str, port: int = 22, timeout: float = 2.0):
        self.address = (socket.gethostbyname(host), port)
        self.timeout = timeout
        self._request_id = 0

    def request(self, command: str) -> str:
        self._request_id = self._request_id % 255 + 1
        rid = self._request_id
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(self.timeout)
            sock.sendto(f"cmd:{rid} {command}\n".encode(), self.address)
            while True:
                reply, peer = sock.recvfrom(4096)
                if peer[0] == self.address[0]:
                    break
        lines = reply.decode("utf-8", "strict").strip().splitlines()
        body = []
        for line in lines:
            head, _, rest = line.partition(" ")
            status, _, got = head.partition(":")
            if got != str(rid):
                raise MPlaneError(f"request-id mismatch: {line}")
            if status == "NOK":
                raise MPlaneError(f"{command}: NOK {rest}")
            body.append(rest)
        return "\n".join(body)
