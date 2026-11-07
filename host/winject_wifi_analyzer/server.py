"""Host web app for the ESP32U IQ server.

Serves web/ over HTTP, talks to the browser over one WebSocket (/ws, JSON),
controls the radio over mplane and turns WJIQ captures into live spectra,
power/tone readings, constellations and sweep tiles.
"""
from __future__ import annotations

import asyncio
from collections import deque
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
from .native_decoder import BACKEND as DECODER_BACKEND
from .legacy_ofdm import SUPPORTED_RATES_MBPS
from .dsss import DsssDecoder

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
                       "rate_hz": 40_000_000, "n": 16380, "bits": 8,
                       "nfft": 1024, "average": 0.6, "detector": "average"}
        self.mode = "idle"  # idle | live | sweep
        self.want_live = True
        self.radio_info: dict[str, str] = {}
        self.latest: Capture | None = None
        self.latest_seen = 0
        self.snapshot: Capture | None = None
        self.avg_spec: np.ndarray | None = None
        self.rate_window: list[float] = []
        self.sweep: dict | None = None
        self.resume_live = False
        self.frame_queue: asyncio.Queue = asyncio.Queue(maxsize=4)
        self.frame_records: deque[dict] = deque(maxlen=200)
        self.decoded_records: deque[dict] = deque(maxlen=100)
        self.frame_generation = 0
        self.detector_dropped = 0
        self.detector_processed = 0
        self.detector_error: str | None = None
        self.decode_queue: asyncio.Queue = asyncio.Queue(maxsize=8)
        self.decoder_attempted = 0
        self.decoder_decoded = 0
        self.decoder_dropped = 0
        self.decoder_error: str | None = None
        self.dsss = DsssDecoder()
        self.dsss_queue: asyncio.Queue = asyncio.Queue(maxsize=4)
        self.dsss_attempted = 0
        self.dsss_decoded = 0
        self.dsss_dropped = 0
        self.dsss_error: str | None = None
        link.on_capture = self.on_capture

    # --- radio ------------------------------------------------------------------

    async def apply_radio(self) -> None:
        c = self.config
        gain = c["gain"] if c["gain"] == "agc" else int(c["gain"])
        await self.link.request(f"iq_set freq={int(c['freq_mhz'])} gain={gain} "
                                f"bw={int(c['bw_mhz'])}")

    async def start_live(self) -> None:
        self.want_live = True
        self.frame_generation += 1
        self.frame_records.clear()
        self.decoded_records.clear()
        await self.broadcast({"event": "frames.clear", "data": {}})
        c = self.config
        await self.link.request("iq_stop")
        self.avg_spec = None
        await self.link.request(f"iq_capture n={int(c['n'])} rate={int(c['rate_hz'])} "
                                f"bits={int(c['bits'])} count=0 dst={self.link.dst}")
        self.mode = "live"

    async def stop(self) -> None:
        self.want_live = False
        self.frame_generation += 1
        await self.link.request("iq_stop")
        self.mode = "idle"

    async def start_sweep(self, req: dict) -> None:
        self.frame_generation += 1
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
        if self.mode == "live" and cap.sample_rate_hz == 40_000_000:
            if self.dsss.available:
                if self.dsss_queue.full():
                    self.dsss_queue.get_nowait()
                    self.dsss_dropped += 1
                self.dsss_queue.put_nowait((self.frame_generation, cap))
            if self.frame_queue.full():
                self.frame_queue.get_nowait()
                self.detector_dropped += 1
            self.frame_queue.put_nowait((self.frame_generation, cap))

    async def frame_loop(self) -> None:
        # Capture callbacks never wait on DSP: under load the bounded queue
        # discards the oldest unprocessed burst and reports that count.
        from .frame_detector import detect_frames
        while True:
            generation, cap = await self.frame_queue.get()
            if generation != self.frame_generation or self.mode != "live":
                continue
            try:
                frames = await asyncio.to_thread(detect_frames, cap.samples(),
                                                 cap.sample_rate_hz)
            except Exception as exc:
                log.exception("frame detector failed")
                self.detector_error = str(exc)
                continue
            if generation != self.frame_generation or self.mode != "live":
                continue
            self.detector_processed += 1
            if not frames:
                continue
            records = []
            for frame in frames:
                record = {**frame, "capture_id": cap.capture_id,
                          "timestamp_us": cap.timestamp_us,
                          "frequency_hz": cap.frequency_hz,
                          "sample_rate_hz": cap.sample_rate_hz}
                self.frame_records.append(record)
                records.append(record)
                # The legacy decoder is slower than acquisition. Queue only
                # stronger candidates and report all dropped decode work.
                if frame["short_score"] >= .90 and frame["long_score"] >= .90:
                    if self.decode_queue.full():
                        self.decode_queue.get_nowait()
                        self.decoder_dropped += 1
                    self.decode_queue.put_nowait((generation, cap, record))
            await self.broadcast({"event": "frames", "data": {"frames": records}})

    async def decode_loop(self) -> None:
        from .legacy_ofdm import decode_legacy_ofdm
        while True:
            generation, cap, record = await self.decode_queue.get()
            if generation != self.frame_generation or self.mode != "live":
                continue
            try:
                result = await asyncio.to_thread(
                    decode_legacy_ofdm, cap.samples(), record["start_sample"],
                    record["long_start_sample"], cap.sample_rate_hz)
            except Exception as exc:
                log.exception("legacy OFDM decoder failed")
                self.decoder_error = str(exc)
                continue
            self.decoder_attempted += 1
            if result is None or generation != self.frame_generation or self.mode != "live":
                continue
            self.decoder_decoded += 1
            record.update(result)
            record["status"] = "FCS verified"
            record["reason"] = f"Legacy {result.get('rate_mbps', '')} Mbit/s OFDM MPDU decoded; FCS verified."
            self.decoded_records.append(record)
            await self.broadcast({"event": "frames", "data": {"frames": [record]}})

    async def dsss_loop(self) -> None:
        # A bounded, independent queue ensures capture gaps are not joined and
        # keeps the optional subprocess away from the network event loop.
        while True:
            generation, cap = await self.dsss_queue.get()
            if generation != self.frame_generation or self.mode != "live":
                continue
            try:
                frames = await asyncio.to_thread(self.dsss.decode, cap.samples(),
                                                 cap.sample_rate_hz, cap.frequency_hz)
            except Exception as exc:
                log.exception("DSSS/CCK decoder failed")
                self.dsss_error = str(exc)
                continue
            self.dsss_error = None
            self.dsss_attempted += 1
            if generation != self.frame_generation or self.mode != "live":
                continue
            records = []
            for frame in frames:
                record = {**frame, "capture_id": cap.capture_id,
                          "timestamp_us": cap.timestamp_us,
                          "frequency_hz": cap.frequency_hz,
                          "sample_rate_hz": cap.sample_rate_hz,
                          "rate_mbps": frame["rate_bps"] / 1e6,
                          "status": "FCS verified"}
                self.dsss_decoded += 1
                self.decoded_records.append(record)
                records.append(record)
            if records:
                await self.broadcast({"event": "frames", "data": {"frames": records}})

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
            "completed": a.completed, "lost": a.lost, "errors": a.errors,
            "detector_processed": self.detector_processed,
            "detector_dropped": self.detector_dropped,
            "detector_supported": int(self.config["rate_hz"]) == 40_000_000,
            "detector_sample_rates_hz": [40_000_000],
            "decoder_backend": ("C++ Viterbi/descrambler, NumPy DSP" if DECODER_BACKEND == "native-cpp" else "Python/NumPy"),
            "decoder_formats": ["legacy-ofdm"] + (["DSSS", "CCK"] if self.dsss.available else []),
            "decoder_rates_mbps": ([1, 2, 5.5, 11] if self.dsss.available else []) + list(SUPPORTED_RATES_MBPS),
            "dsss_attempted": self.dsss_attempted,
            "dsss_decoded": self.dsss_decoded,
            "dsss_dropped": self.dsss_dropped,
            "dsss_error": self.dsss_error,
            "detector_error": self.detector_error,
            "decoder_attempted": self.decoder_attempted,
            "decoder_decoded": self.decoder_decoded,
            "decoder_dropped": self.decoder_dropped,
            "decoder_error": self.decoder_error}}

    async def handle(self, ws: WebSocketServerProtocol, path: str) -> None:
        self.clients.add(ws)
        try:
            await ws.send(json.dumps(self.status()))
            if self.frame_records or self.decoded_records:
                # Preserve verified MPDUs independently of the much larger
                # stream of unverified preamble candidates.
                records = {(f["capture_id"], f["start_sample"]): f
                           for f in self.frame_records}
                records.update({(f["capture_id"], f["start_sample"]): f
                                for f in self.decoded_records})
                await ws.send(json.dumps({"event": "frames", "data": {
                    "frames": list(records.values()), "history": True}}))
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
                    if self.want_live and self.mode != "sweep" and self.radio_info.get("kind") == "idle":
                        log.info("radio is idle; restoring the requested live stream")
                        await self.apply_radio()
                        if self.want_live:
                            await self.start_live()
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
        detector = asyncio.create_task(app.frame_loop())
        decoder = asyncio.create_task(app.decode_loop())
        dsss_decoder = asyncio.create_task(app.dsss_loop())
        try:
            await app.ui_loop()
        finally:
            detector.cancel()
            decoder.cancel()
            dsss_decoder.cancel()
            await asyncio.gather(detector, decoder, dsss_decoder, return_exceptions=True)
