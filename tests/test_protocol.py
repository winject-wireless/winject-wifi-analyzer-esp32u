import unittest
import zlib

from winject_wifi_analyzer.protocol import CaptureAssembler, FINAL, HEADER, ProtocolError


def frame(capture_id, samples, bits, rate, offset, payload, complete, crc):
    return HEADER.pack(b"WJIQ", 1, FINAL if complete else 0, bits, rate,
                       capture_id, samples, offset, len(payload), HEADER.size, crc) + payload


class ProtocolTest(unittest.TestCase):
    def test_reassembles_out_of_order_and_duplicate(self):
        payload = bytes(range(16))
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        assembler = CaptureAssembler()
        self.assertIsNone(assembler.add(frame(7, 8, 8, 6, 8, payload[8:], True, crc)))
        self.assertIsNone(assembler.add(frame(7, 8, 8, 6, 8, payload[8:], True, crc)))
        capture = assembler.add(frame(7, 8, 8, 6, 0, payload[:8], False, crc))
        self.assertEqual(capture.payload, payload)

    def test_rejects_bad_crc(self):
        payload = bytes(range(8))
        with self.assertRaisesRegex(ProtocolError, "CRC mismatch"):
            CaptureAssembler().add(frame(3, 4, 8, 0, 0, payload, True, 0))

    def test_rejects_truncated_datagram(self):
        with self.assertRaisesRegex(ProtocolError, "truncated"):
            CaptureAssembler().add(b"WJIQ")

    def test_iq10_odd_sample_size(self):
        payload = b"12345678"
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        capture = CaptureAssembler().add(frame(9, 3, 10, 1, 0, payload, True, crc))
        self.assertEqual(capture.payload, payload)


if __name__ == "__main__":
    unittest.main()
