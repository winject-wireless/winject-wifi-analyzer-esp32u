# Optional DSSS/CCK receiver

The host can use the separately built MIT-licensed
[libcrafter](https://github.com/pellegre/libcrafter) Rust `radio_receive`
executable through `winject_wifi_analyzer.dsss.DsssDecoder`. The adapter does
not bundle upstream code, is not a C++ implementation, and is optional. It
only returns 802.11b DSSS/CCK MPDUs with a valid FCS, which the adapter verifies
again. A 40 MS/s capture is low-pass filtered and downsampled to the upstream
decoder's 20 MS/s input. Each capture is replayed independently: the receiver
is never given falsely contiguous data across ESP32U capture gaps.

## Build and enable

Rust >= 1.89, Cargo, a C toolchain and libpcap development headers are needed.
Build from the pinned MIT-licensed upstream revision:

```sh
git clone https://github.com/pellegre/libcrafter.git libcrafter-iq
cd libcrafter-iq
git checkout 26347a513bfbf0f5f23a330a83c87ce1ba4667fc
cargo build -p crafter --release --features radio --example radio_receive
export WINJECT_CRAFTER_RADIO_RECEIVE="$PWD/target/release/examples/radio_receive"
```

On the current OPi5, the tested executable is installed at
`/home/ubuntu/.local/bin/winject-crafter-radio-receive`; its MIT license and
source revision are retained under `/home/ubuntu/.local/share/winject-crafter/`.
Set the service variable to that path to use this installed copy.

Export that variable in the environment that starts the host service. The
adapter exposes `DsssDecoder().available` and
`DsssDecoder().decode(samples, sample_rate_hz, frequency_hz)`. It raises
`DsssUnavailable` when the executable is missing, times out or fails; an empty
list means no FCS-valid DSSS/CCK frame was recovered. Samples are normalized
complex64 in conventional I+jQ order. Frames include raw Addr1/Addr2/Addr3,
MPDU hex excluding FCS, rate, modulation and approximate burst sample index.
The adapter intentionally ignores any OFDM frames the upstream combined
receiver also emits, to avoid duplicates with the separate OFDM decoder.

The adapter itself does not enable or schedule decoding in the service. It
must be called from a bounded background worker; report queue drops and never
block the multicast receive loop. On this OPi5 a 50-run replay of one real
16,380-sample 40 MS/s burst took a median 17.92 ms and p95 18.57 ms per burst,
including process startup and filtering. A single serial worker therefore
cannot process the approximately 195 bursts/s produced by the current source;
parallel workers or deliberate downsampling of bursts are required and must be
measured. Current burst duration is about 409.5 µs, so frames crossing a gap or
longer than a burst cannot be completely decoded.

## Verification

The adapter has basic unit tests without an upstream checkout. The integration
suite uses upstream's independently generated test vectors and SciPy, without
copying them into this repository:

```sh
export WINJECT_CRAFTER_FIXTURES="$PWD/crafter/tests/fixtures/iq"
cd /path/to/winject-wifi-analyzer-esp32u
PYTHONPATH=host python3 -m unittest discover -s tests -p 'test_dsss.py' -v
```

The tests exercise clean 1/2/5.5/11 Mb/s waveforms, FCS failure rejection,
40-to-20 MS/s conversion for frames fitting one ESP32 capture, and truncation
of a longer 1 Mb/s frame. A separate upstream replay of its 32 DSSS/CCK vector
matrix passed all 32 cases. These are synthetic vectors; successful decoding of
an over-the-air 802.11b frame from this ESP32U has not yet been demonstrated.
