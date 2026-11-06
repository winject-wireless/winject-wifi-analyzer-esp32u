"""Host web app for the ESP32U IQ server.

Serves web/ over HTTP, talks to the browser over one WebSocket (/ws, JSON),
controls the radio over mplane and turns WJIQ captures into live spectra,
power/tone readings, constellations and sweep tiles.
"""
from __future__ import annotations

import asyncio
import json
import logging
import mimetypes
import time
from dataclasses import asdict
from http import HTTPStatus
from pathlib import Path

import numpy as np
import websockets
from websockets.legacy.server import WebSocketServerProtocol

from . import dsp
from .protocol import (Capture, FLAG_LAST_CAPTURE, FLAG_LAST_IN_STEP,
                       FLAG_LAST_STEP)
from .radio import MPlaneError, RadioLink

log = logging.getLogger("analyzer")
WEB_DIR = Path(__file__).resolve().parents[2] / "web"
RATES = (80_000_000, 40_000_000, 16_000_000)


def _points(x: np.ndarray) -> list[list[float]]:
    return np.round(np.stack([x.real, x.imag], axis=1), 4).tolist()


class Analyzer:
    def __init__(self, link: RadioLink, ui_hz: float = 12.0) -> None:
        self.link = link
        self.ui_period = 1.0 / ui_hz
        self.clients: set[WebSocketServerProtocol] = set()
        self.config = {"freq_mhz": 2437, "gain": "agc", "bw_mhz": 0,
                       "rate_hz": 16_000_000, "n": 16380, "bits": 8,
                       "nfft": 1024, "average": 0.6, "detector": "average"}
        self.mode = "idle"  # idle | live | sweep
        self.radio_info: dict[str, str] = {}
        self.latest: Capture | None = None
        self.latest_seen = 0
        self.snapshot: Capture | None = None
        self.avg_spec: np.ndarray | None = None
        self.rate_window: list[float] = []
        self.sweep: dict | None = None
        self.resume_live = False
        link.on_capture = self.on_capture

    # --- radio ------------------------------------------------------------------

    async def apply_radio(self) -> None:
        c = self.config
        gain = c["gain"] if c["gain"] == "agc" else int(c["gain"])
        await self.link.request(f"iq_set freq={int(c['freq_mhz'])} gain={gain} "
                                f"bw={int(c['bw_mhz'])}")

    async def start_live(self) -> None:
        c = self.config
        await self.link.request("iq_stop")
        self.avg_spec = None
        await self.link.request(f"iq_capture n={int(c['n'])} rate={int(c['rate_hz'])} "
                                f"bits={int(c['bits'])} count=0 dst={self.link.dst}")
        self.mode = "live"

    async def stop(self) -> None:
        await self.link.request("iq_stop")
        self.mode = "idle"

    async def start_sweep(self, req: dict) -> None:
        start, stop = int(req["start_mhz"]), int(req["stop_mhz"])
        step = int(req.get("step_mhz", 10))
        rate = int(req.get("rate_hz", 16_000_000))
        nfft = int(req.get("fft", 512))
        dwell = int(req.get("dwell", 4))
        if rate not in RATES or not 100 <= start <= stop <= 6000 or step < 1:
            raise ValueError("start/stop 100..6000 MHz, rate 80/40/16 MS/s")
        if step * 1e6 > 0.8 * rate:
            raise ValueError("step must be <= 0.8 x sample rate")
        self.resume_live = self.mode == "live"
        await self.link.request("iq_stop")
        # Fixed gain (per-step AGC would scale every tile differently) and an
        # analog low-pass at ~0.8 x fs; both restored after the sweep.
        gain = self.config["gain"]
        bw = 0 if rate >= 80_000_000 else max(12, min(67, round(rate * 0.8 / 1e6)))
        await self.link.request(f"iq_set gain={40 if gain == 'agc' else int(gain)} bw={bw}")
        repeat = bool(req.get("repeat", False))
        reply = await self.link.request(
            f"iq_sweep start={start} stop={stop} step={step} n={max(nfft, 1024)} "
            f"rate={rate} bits=8 dwell={dwell} passes={0 if repeat else 1} "
            f"dst={self.link.dst}")
        job = int(dict(p.split("=", 1) for p in reply[0].split()[1:] if "=" in p)["id"])
        self.sweep = {"job": job, "step_hz": step * 1e6, "nfft": nfft,
                      "peak": req.get("detector") == "peak", "pending": [],
                      "steps": (stop - start) // step + 1, "start_mhz": start,
                      "stop_mhz": stop, "step_mhz": step, "t0": time.monotonic()}
        self.mode = "sweep"
        await self.broadcast({"event": "sweep.start", "data": {
            k: self.sweep[k] for k in ("job", "steps", "start_mhz", "stop_mhz", "step_mhz")}})

    async def end_sweep(self, state: str) -> None:
        if self.sweep is None:
            return
        info = {"job": self.sweep["job"], "state": state,
                "seconds": round(time.monotonic() - self.sweep["t0"], 2)}
        self.sweep = None
        await self.link.request("iq_stop")
        await self.apply_radio()
        self.mode = "idle"
        if self.resume_live:
            await self.start_live()
        await self.broadcast({"event": "sweep.done", "data": info})

    # --- captures -----------------------------------------------------------------

    def on_capture(self, cap: Capture) -> None:
        now = time.monotonic()
        self.rate_window.append(now)
        if self.mode == "sweep" and self.sweep is not None and cap.job_id == self.sweep["job"]:
            self._sweep_capture(cap)
            return
        self.latest = cap

    def _sweep_capture(self, cap: Capture) -> None:
        s = self.sweep
        s["pending"].append(cap.samples())
        if not cap.flags & FLAG_LAST_IN_STEP:
            return
        tile = dsp.sweep_tile(s["pending"], cap.sample_rate_hz, cap.frequency_hz,
                              s["step_hz"], s["nfft"], s["peak"])
        s["pending"] = []
        tile.update(index=cap.step, total=cap.steps, gain_index=cap.gain_index)
        asyncio.ensure_future(self.broadcast({"event": "sweep.tile", "data": tile}))
        if cap.flags & FLAG_LAST_CAPTURE:
            asyncio.ensure_future(self.end_sweep("completed"))

    def live_frame(self, cap: Capture) -> dict:
        x = cap.samples()
        fs = cap.sample_rate_hz
        nfft = int(self.config["nfft"])
        freqs, spec = dsp.psd(x, fs, nfft, self.config["detector"] == "peak")
        a = float(self.config["average"])
        if self.avg_spec is None or len(self.avg_spec) != len(spec) or a <= 0:
            self.avg_spec = spec
        else:
            lin = a * 10 ** (self.avg_spec / 10) + (1 - a) * 10 ** (spec / 10)
            self.avg_spec = dsp.db(lin).astype(np.float32)
        st = dsp.power_stats(x, fs)
        return {
            "capture_id": cap.capture_id, "frequency_hz": cap.frequency_hz,
            "sample_rate_hz": fs, "samples": cap.sample_count, "bits": cap.bits,
            "gain_index": cap.gain_index, "agc": cap.agc,
            "spectrum": {"start_hz": cap.frequency_hz + float(freqs[0]),
                         "bin_hz": fs / nfft,
                         "db": np.round(self.avg_spec, 1).tolist()},
            "power": {**{k: round(v, 2) for k, v in asdict(st).items()},
                      "tone_hz": cap.frequency_hz + st.tone_offset_hz},
            "envelope": np.round(dsp.envelope(x, 512), 1).tolist(),
            "points": _points(dsp.decimate_points(x, 512)),
        }

    # --- browser -------------------------------------------------------------------

    async def broadcast(self, msg: dict) -> None:
        if not self.clients:
            return
        text = json.dumps(msg, separators=(",", ":"))
        await asyncio.gather(*(c.send(text) for c in list(self.clients)),
                             return_exceptions=True)

    def status(self) -> dict:
        now = time.monotonic()
        self.rate_window = [t for t in self.rate_window if now - t < 2.0]
        a = self.link.assembler
        return {"event": "status", "data": {
            "mode": self.mode, "config": self.config, "radio": self.radio_info,
            "radio_ip": self.link.radio_ip, "captures_per_s": round(len(self.rate_window) / 2, 1),
            "datagrams": self.link.datagrams, "mbytes": round(self.link.bytes / 1e6, 1),
            "completed": a.completed, "lost": a.lost, "errors": a.errors}}

    async def handle(self, ws: WebSocketServerProtocol, path: str) -> None:
        self.clients.add(ws)
        try:
            await ws.send(json.dumps(self.status()))
            async for raw in ws:
                try:
                    await self.command(json.loads(raw))
                except (MPlaneError, ValueError, KeyError) as exc:
                    await ws.send(json.dumps({"event": "error", "data": {"message": str(exc)}}))
                await ws.send(json.dumps(self.status()))
        except websockets.ConnectionClosed:
            pass
        finally:
            self.clients.discard(ws)

    async def command(self, msg: dict) -> None:
        op = msg.get("op")
        if op == "config":
            for k in ("freq_mhz", "gain", "bw_mhz", "rate_hz", "n", "bits", "nfft",
                      "average", "detector"):
                if k in msg:
                    self.config[k] = msg[k]
            if int(self.config["rate_hz"]) not in RATES:
                raise ValueError("rate must be 80, 40 or 16 MS/s")
            if self.mode != "sweep":
                await self.apply_radio()
                if self.mode == "live":
                    await self.start_live()
        elif op == "live":
            if msg.get("on", True):
                await self.apply_radio()
                await self.start_live()
            else:
                await self.stop()
        elif op == "snapshot":
            if self.latest is None:
                raise ValueError("no capture yet: start live first")
            self.snapshot = self.latest
            x = self.snapshot.samples()
            await self.broadcast({"event": "snapshot", "data": {
                "capture_id": self.snapshot.capture_id,
                "frequency_hz": self.snapshot.frequency_hz,
                "sample_rate_hz": self.snapshot.sample_rate_hz,
                "samples": self.snapshot.sample_count, "bits": self.snapshot.bits,
                "points": _points(dsp.decimate_points(x, 4096)),
                "envelope": np.round(dsp.envelope(x, 1024), 1).tolist(),
                "download": "/capture/snapshot.cf32"}})
        elif op == "sweep":
            await self.start_sweep(msg)
        elif op == "stop_sweep":
            await self.end_sweep("cancelled")
        else:
            raise ValueError(f"unknown op {op!r}")

    # --- loops ------------------------------------------------------------------------

    async def ui_loop(self) -> None:
        last_status = 0.0
        while True:
            await asyncio.sleep(self.ui_period)
            cap = self.latest
            if self.mode == "live" and cap is not None and cap.capture_id != self.latest_seen:
                self.latest_seen = cap.capture_id
                await self.broadcast({"event": "live", "data": self.live_frame(cap)})
            now = time.monotonic()
            if now - last_status >= 1.0:
                last_status = now
                try:
                    self.radio_info = await self.link.info()
                except MPlaneError as exc:
                    self.radio_info = {"error": str(exc)}
                await self.broadcast(self.status())

    def http(self, path: str, headers):
        if path == "/ws":
            return None
        if path.startswith("/capture/snapshot"):
            cap = self.snapshot
            if cap is None:
                return HTTPStatus.NOT_FOUND, [], b"no snapshot\n"
            if path.endswith(".json"):
                meta = {k: getattr(cap, k) for k in (
                    "capture_id", "sample_count", "bits", "frequency_hz", "timestamp_us",
                    "gain_index", "agc", "bandwidth_mhz")}
                meta.update(sample_rate_hz=cap.sample_rate_hz, format="cf32_le",
                            scale="|x| = 1 at 0 dBFS")
                return HTTPStatus.OK, [("Content-Type", "application/json")], \
                    json.dumps(meta).encode()
            name = f"esp32u_{cap.frequency_hz // 1000000}MHz_{cap.sample_rate_hz // 1000000}Msps_{cap.capture_id}.cf32"
            return HTTPStatus.OK, [("Content-Type", "application/octet-stream"),
                                   ("Content-Disposition", f"attachment; filename={name}")], \
                cap.samples().tobytes()
        rel = "index.html" if path in ("/", "") else path.lstrip("/").split("?")[0]
        f = (WEB_DIR / rel).resolve()
        if WEB_DIR not in f.parents or not f.is_file():
            return HTTPStatus.NOT_FOUND, [], b"not found\n"
        ctype = mimetypes.guess_type(f.name)[0] or "application/octet-stream"
        return HTTPStatus.OK, [("Content-Type", ctype), ("Cache-Control", "no-cache")], \
            f.read_bytes()


async def serve(radio: str, http_host: str, http_port: int, data_port: int) -> None:
    # websockets logs every plain HTTP (static file) request as a failed handshake.
    logging.getLogger("websockets.server").setLevel(logging.WARNING)
    link = RadioLink(radio, data_port=data_port)
    await link.start()
    app = Analyzer(link)
    try:
        app.radio_info = await link.info()
        await app.apply_radio()
        await app.start_live()
    except MPlaneError as exc:
        log.warning("radio not ready: %s", exc)
    async with websockets.serve(app.handle, http_host, http_port,
                                process_request=app.http, max_size=1 << 20):
        log.info("web app on http://%s:%d/ (radio %s, samples -> %s)",
                 http_host, http_port, link.radio_ip, link.dst)
        await app.ui_loop()
