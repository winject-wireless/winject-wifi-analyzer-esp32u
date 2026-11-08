The 20 MS/s IQ vector ag_6M_len14_pre100_post200_openwifi.txt and
ag_54M_len14_pre100_post200_openwifi.txt are from
OpenOFDM (https://github.com/jhshi/openofdm), commit ea638eb, Apache-2.0
(see LICENSE.txt). It contain independently generated 6 Mbps and 54 Mbps legacy OFDM
PPDUs with 14 PSDU bytes. The PSDU is test data, not a valid 802.11 MAC frame;
the test verifies its independently generated payload and FCS and stubs only
the separate MAC-header parser.

The dot11a_*mbps_qos_data_*.dat vectors are from OpenOFDM commit ea638eb,
testing_inputs/conducted/. They contain 20 MS/s little-endian interleaved
signed 16-bit I/Q captured over a conducted RF path. The decoder tests confirm
FCS-valid frames at the expected rates after interpolation to 40 MS/s.

dot11a_24M_40msps_iq8.cs8 is generated from the adjacent 24 Mbps conducted
RF fixture by resampling to 40 MS/s, normalizing by 32768, quantizing signed
I/Q to 8 bits and taking the first 16380 samples. It exercises the WJIQ-to-
decoder integration test.
