"""Parse an already-demodulated, FCS-bearing 802.11 MPDU.

This module does not demodulate IQ. Never call a preamble candidate a decoded
frame: a PHY decoder must first deliver complete bytes including the FCS.
"""
from __future__ import annotations

import zlib


def parse_mpdu(frame: bytes) -> dict | None:
    """Return MAC metadata only when the complete MPDU passes CRC-32/FCS.

    Address fields retain their raw 802.11 names; their semantic meaning
    depends on type and DS flags. Unsupported and truncated headers return None.
    The FCS is supplied in little-endian wire order and excluded from mpdu_hex.
    """
    if len(frame) < 14:
        return None
    mpdu, received_fcs = frame[:-4], int.from_bytes(frame[-4:], 'little')
    if zlib.crc32(mpdu) & 0xffffffff != received_fcs:
        return None
    fc = int.from_bytes(mpdu[:2], 'little')
    if fc & 3:  # only protocol version 0
        return None
    typ, subtype = (fc >> 2) & 3, (fc >> 4) & 15
    tods, fromds = bool(fc & 0x0100), bool(fc & 0x0200)
    if typ == 0:  # management
        if len(mpdu) < 24:
            return None
        header_len = 24 + (4 if fc & 0x8000 else 0)
    elif typ == 1:
        # ACK/CTS contain only RA; reject control wrappers/extensions whose
        # body layout doesn't have a normal second address.
        control_header_lengths = {8: 20, 9: 28, 10: 16, 11: 16,
                                  12: 10, 13: 10, 14: 16, 15: 16}
        if subtype not in control_header_lengths:
            return None
        header_len = control_header_lengths[subtype]
    elif typ == 2:  # data, optional fourth address, QoS, HT control
        header_len = 24 + (6 if tods and fromds else 0)
        if subtype & 8:
            header_len += 2
            if fc & 0x8000:
                header_len += 4
    else:
        return None  # extension frame layout is not implemented
    if len(mpdu) < header_len:
        return None

    def address(offset: int) -> str:
        return ':'.join(f'{b:02x}' for b in mpdu[offset:offset + 6])

    result = {
        'mpdu_hex': mpdu.hex(), 'fcs_ok': True, 'frame_type': typ,
        'frame_subtype': subtype, 'to_ds': tods, 'from_ds': fromds,
        'protected': bool(fc & 0x4000), 'header_length': header_len,
        'addr1': address(4),
        'addr2': address(10) if typ != 1 or header_len >= 16 else None,
        'addr3': address(16) if typ in (0, 2) else None,
        'addr4': address(24) if typ == 2 and tods and fromds else None,
    }
    if typ in (0, 2):
        sequence_control = int.from_bytes(mpdu[22:24], 'little')
        result.update(sequence=sequence_control >> 4,
                      fragment=sequence_control & 15)
    return result
