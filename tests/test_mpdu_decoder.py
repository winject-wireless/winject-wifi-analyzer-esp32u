import unittest
import zlib

from winject_wifi_analyzer.mpdu_decoder import parse_mpdu


def with_fcs(data):
    return data + (zlib.crc32(data) & 0xffffffff).to_bytes(4, 'little')


class MPDUParserTest(unittest.TestCase):
    def test_beacon_fields_and_fcs(self):
        raw = bytes.fromhex('80000000 ffffffffffff 102030405060 aabbccddeeff 3112') + b'beacon'
        result = parse_mpdu(with_fcs(raw))
        self.assertEqual(result['addr1'], 'ff:ff:ff:ff:ff:ff')
        self.assertEqual(result['addr2'], '10:20:30:40:50:60')
        self.assertEqual(result['addr3'], 'aa:bb:cc:dd:ee:ff')
        self.assertEqual(result['sequence'], 0x123)
        self.assertEqual(result['fragment'], 1)
        self.assertEqual(result['mpdu_hex'], raw.hex())
        corrupt = bytearray(with_fcs(raw)); corrupt[8] ^= 1
        self.assertIsNone(parse_mpdu(bytes(corrupt)))

    def test_ack_and_short_frame(self):
        ack = bytes.fromhex('d4000000 102030405060')
        result = parse_mpdu(with_fcs(ack))
        self.assertEqual(result['addr1'], '10:20:30:40:50:60')
        self.assertIsNone(result['addr2'])
        self.assertIsNone(result['addr3'])
        self.assertIsNone(parse_mpdu(with_fcs(ack[:-1])))

    def test_four_address_qos_ht_header_length(self):
        data = bytes.fromhex('88030000 010203040506 111213141516 212223242526 0000 313233343536 0000')
        r = parse_mpdu(with_fcs(data))
        self.assertEqual(r['header_length'], 32)
        self.assertEqual(r['addr4'], '31:32:33:34:35:36')
        self.assertIsNone(parse_mpdu(with_fcs(data[:-1])))

    def test_reject_unknown_protocol(self):
        self.assertIsNone(parse_mpdu(with_fcs(b'\x81\x00' + bytes(22))))


if __name__ == '__main__':
    unittest.main()
