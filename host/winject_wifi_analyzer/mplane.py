from __future__ import annotations

import socket


class MPlaneError(RuntimeError):
    pass


class MPlaneClient:
    def __init__(self, host: str, port: int = 22, timeout: float = 2.0):
        self.address = (host, port)
        self.timeout = timeout
        self._request_id = 0

    def request(self, command: str) -> str:
        self._request_id = (self._request_id + 1) & 0xFF
        prefix = f"cmd:{self._request_id}"
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(self.timeout)
            sock.sendto(f"{prefix} {command}\n".encode(), self.address)
            reply, peer = sock.recvfrom(2048)
        if peer[0] != socket.gethostbyname(self.address[0]):
            raise MPlaneError(f"reply from unexpected host {peer[0]}")
        text = reply.decode("utf-8", "strict").strip()
        expected = f"<< {prefix} "
        if not text.startswith(expected):
            raise MPlaneError(f"request-id mismatch: {text}")
        body = text[len(expected):]
        if body.startswith("NOK "):
            raise MPlaneError(body)
        return body
