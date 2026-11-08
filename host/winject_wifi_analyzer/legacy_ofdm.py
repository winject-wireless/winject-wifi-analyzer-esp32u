"""Experimental 20 MHz 802.11 legacy OFDM PHY, 6–54 Mbit/s.

Accepts a complete 40 MS/s complex IQ burst and a candidate's short/long
training offsets. A result is returned only for a complete CRC-valid MPDU;
unsupported rates, truncated captures, invalid SIGNAL and FCS failures return
None. No result can be inferred from a periodicity candidate alone.
"""
from __future__ import annotations

import numpy as np
from .mpdu_decoder import parse_mpdu
from .native_decoder import viterbi as _viterbi
try:
    from ._native import descramble as _native_descramble
except ImportError:
    _native_descramble = None

# Legacy L-LTF frequency-domain BPSK sequence, -26..+26, DC=0.
_LTF = np.array([1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1], dtype=np.float64)
_ACTIVE = np.r_[np.arange(-26, 0), np.arange(1, 27)]
_DATA = np.array([k for k in _ACTIVE if k not in (-21, -7, 7, 21)])
_PILOTS = np.array([-21, -7, 7, 21])
# IEEE 802.11 legacy OFDM pilot polarity sequence.
_POLARITY = np.array([1,1,1,1,-1,-1,-1,1,-1,-1,-1,-1,1,1,-1,1,-1,-1,1,1,-1,1,1,-1,1,1,1,1,1,1,-1,1,1,1,-1,1,1,-1,-1,1,1,1,-1,1,-1,-1,-1,1,-1,1,-1,-1,1,-1,-1,1,1,1,1,1,-1,-1,1,1,-1,-1,1,-1,1,1,-1,-1,-1,1,1,-1,-1,-1,-1,1,-1,-1,1,-1,1,1,1,1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,-1,-1,1,-1,1,-1,1,1,1,-1,-1,1,-1,-1,-1,1,1,1,1,-1,-1,-1,-1,-1,-1,-1], dtype=np.int8)
_REF = _LTF[_LTF != 0]
_LTF_BINS = np.zeros(64, dtype=np.complex128); _LTF_BINS[_ACTIVE % 64] = _REF
_LTF_TIME = np.fft.ifft(_LTF_BINS)
_PERM = np.array([3 * (k % 16) + k // 16 for k in range(48)])
# SIGNAL RATE bits in on-air bit order: modulation, coded/data bits per symbol,
# puncturing mask applied to the K=7 rate-1/2 mother code.
_RATES = {
    (1,1,0,1): (6, 1, 24, (1,1)),
    (1,1,1,1): (9, 1, 36, (1,1,1,0,0,1)),
    (0,1,0,1): (12, 2, 48, (1,1)),
    (0,1,1,1): (18, 2, 72, (1,1,1,0,0,1)),
    (1,0,0,1): (24, 4, 96, (1,1)),
    (1,0,1,1): (36, 4, 144, (1,1,1,0,0,1)),
    (0,0,0,1): (48, 6, 192, (1,1,1,0)),
    (0,0,1,1): (54, 6, 216, (1,1,1,0,0,1)),
}
SUPPORTED_RATES_MBPS = (6, 9, 12, 18, 24, 36, 48, 54)
_MODULATIONS = {1:'BPSK', 2:'QPSK', 4:'16-QAM', 6:'64-QAM'}

def _deinterleave_map(n_bpsc: int) -> np.ndarray:
    n_cbps=48*n_bpsc; s=max(n_bpsc//2,1); k=np.arange(n_cbps)
    i=(n_cbps//16)*(k%16)+k//16
    return (s*(i//s)+(i+n_cbps-(16*i)//n_cbps)%s).astype(np.int32)

def _constellation(n_bpsc: int) -> tuple[np.ndarray,np.ndarray]:
    bits=np.array([[(i>>(n_bpsc-1-j))&1 for j in range(n_bpsc)]
                   for i in range(1<<n_bpsc)],dtype=np.uint8)
    if n_bpsc==1:return np.array([-1+0j,1+0j]),bits
    axis_bits=n_bpsc//2
    mapping=({1:(-1,1),2:(-3,-1,3,1),3:(-7,-5,-1,-3,7,5,1,3)})[axis_bits]
    vals=[]
    for row in bits:
        i=int(''.join(map(str,row[:axis_bits])),2)
        q=int(''.join(map(str,row[axis_bits:])),2)
        vals.append(mapping[i]+1j*mapping[q])
    return np.asarray(vals,dtype=np.complex128)/np.sqrt({2:2,4:10,6:42}[n_bpsc]),bits

def _hard_demap(samples: np.ndarray,n_bpsc: int):
    points,bits=_constellation(n_bpsc)
    nearest=np.argmin(np.abs(samples[:,None]-points[None,:])**2,axis=1)
    return bits[nearest].reshape(-1),points[nearest]

def _depuncture(bits: np.ndarray,pattern: tuple[int,...]) -> np.ndarray:
    if pattern==(1,1):return bits
    # Full transmitted symbols always contain an integer number of patterns.
    count=int(np.count_nonzero(pattern))
    if len(bits)%count:raise ValueError('invalid punctured bit count')
    out=np.full(len(bits)//count*len(pattern),2,dtype=np.uint8)
    mask=np.tile(np.asarray(pattern,dtype=bool),len(bits)//count)
    out[mask]=bits
    return out


def _descramble(bits: np.ndarray) -> np.ndarray | None:
    if _native_descramble is not None:
        result = _native_descramble(np.ascontiguousarray(bits, dtype=np.uint8))
        return None if result is None else np.frombuffer(result, dtype=np.uint8)
    # SERVICE starts with 16 zeros; find the unique 7-bit scrambler state.
    candidates=[]
    for seed in range(1,128):
        state=[(seed >> j)&1 for j in range(7)]; pn=[]
        for _ in range(16):
            feedback=state[6]^state[3];pn.append(feedback);state=[feedback]+state[:-1]
        if np.array_equal(bits[:16],pn):candidates.append(seed)
    if len(candidates)!=1:return None
    state=[(candidates[0] >> j)&1 for j in range(7)];out=np.empty(len(bits),np.uint8)
    for i,b in enumerate(bits):
        feedback=state[6]^state[3];out[i]=int(b)^feedback;state=[feedback]+state[:-1]
    return out


def _find_ltf(x: np.ndarray, approximate: int) -> tuple[int,float] | None:
    lo=max(0,approximate-96);hi=min(len(x)-128,approximate+96)
    if hi<=lo:return None
    # Two adjacent long symbols should both resemble the known training field.
    corr=np.correlate(x[lo:hi+128],_LTF_TIME,'valid')
    scores=np.abs(corr[:hi-lo])+np.abs(corr[64:64+hi-lo])
    idx=int(np.argmax(scores));pos=lo+idx
    energy=np.vdot(x[pos:pos+64],x[pos:pos+64]).real
    norm=float(abs(corr[idx])/(np.sqrt(energy*np.vdot(_LTF_TIME,_LTF_TIME).real)+1e-15))
    return pos,norm


def decode_legacy_ofdm(samples: np.ndarray, short_start: int, long_start: int,
                        sample_rate_hz: int = 40_000_000) -> dict | None:
    """Decode one complete, FCS-valid 20 MHz legacy OFDM MPDU at 6–54 Mbps."""
    if sample_rate_hz!=40_000_000:return None
    x=np.asarray(samples,dtype=np.complex64).reshape(-1)
    raw_x=x
    if short_start<0 or short_start+288>len(x) or long_start<0:return None
    short=x[short_start:short_start+256]
    phase=float(np.angle(np.vdot(short[:-32],short[32:]))/32)
    x=x*np.exp(-1j*phase*np.arange(len(x)))
    for decimation_phase in (0,1):
        z=x[decimation_phase::2];loc=_find_ltf(z,max(0,(long_start-decimation_phase)//2))
        if loc is None:continue
        first,score=loc
        if score<.20 or first+144+80>len(z):continue
        # Fine CFO from the two long symbols.
        fine=float(np.angle(np.vdot(z[first:first+64],z[first+64:first+128]))/64)
        z=z*np.exp(-1j*fine*(np.arange(len(z))-first))
        y1=np.fft.fft(z[first:first+64]);y2=np.fft.fft(z[first+64:first+128]);h=(y1+y2)/2
        h[_ACTIVE%64]*=_REF
        active_h=h[_ACTIVE%64]
        if np.min(abs(active_h))<1e-12:continue
        def symbol(start, pol):
            if start+80>len(z):return None
            y=np.fft.fft(z[start+16:start+80]);eq=y/np.where(abs(h)>1e-12,h,1)
            pilot_ref=_POLARITY[pol%127]*np.array([1,1,1,-1])
            pilot=eq[_PILOTS%64]*pilot_ref
            if abs(sum(pilot))<1e-12:return None
            eq*=np.exp(-1j*np.angle(sum(pilot)))
            return eq[_DATA%64]
        sig=symbol(first+128,0)
        if sig is None:continue
        # This convention follows the usual BPSK 0->-1 mapping.
        bits=(sig.real>0).astype(np.uint8)[_PERM];decoded=_viterbi(bits,True)
        rate_info=_RATES.get(tuple(decoded[:4]))
        if rate_info is None or decoded[4]!=0 or np.sum(decoded[:18])%2 or np.any(decoded[18:24]):continue
        rate,n_bpsc,n_dbps,puncture=rate_info
        length=sum(int(decoded[5+i])<<i for i in range(12))
        if length<14 or length>4095:continue
        symbols=(16+length*8+6+n_dbps-1)//n_dbps
        if first+128+80*(symbols+1)>len(z):continue
        raw=[];constellation=[]; ideals=[];perm=_deinterleave_map(n_bpsc)
        for n in range(symbols):
            q=symbol(first+208+n*80,n+1)
            if q is None:break
            demapped,ideal=_hard_demap(q,n_bpsc)
            constellation.extend(q.tolist());ideals.extend(ideal.tolist());raw.extend(demapped[perm].tolist())
        else:
            out=_descramble(_viterbi(_depuncture(np.asarray(raw,np.uint8),puncture),False))
            if out is None or np.any(out[:16]):continue
            octets=np.packbits(out[16:16+length*8],bitorder='little').tobytes()
            mpdu=parse_mpdu(octets)
            if mpdu is None:continue
            q=np.asarray(constellation);ideal=np.asarray(ideals)
            evm=float(100*np.sqrt(np.mean(abs(q-ideal)**2)/np.mean(abs(ideal)**2)))
            # Samples from the analyzer are normalized to the ADC range.
            # FCS can still pass with clipping, but the EVM must not be
            # presented as a reliable measurement of that waveform.
            raw_start=first*2+decimation_phase
            raw_end=min(len(raw_x),(first+128+80*(symbols+1))*2+decimation_phase)
            segment=raw_x[raw_start:raw_end]
            clipped=float(np.mean((np.abs(segment.real)>=.98) | (np.abs(segment.imag)>=.98)))
            evm_value=None if clipped>.005 else round(evm,2)
            warning=('IQ samples clipped; data-field EVM estimate withheld' if evm_value is None
                     else 'Decision-directed data-field EVM estimate, including SERVICE and padding; not standards-calibrated')
            return {**mpdu,'modulation':_MODULATIONS[n_bpsc],'rate_mbps':rate,'evm_percent':evm_value,
                    'evm_scope':'data-field decisions including SERVICE and padding; not standards-calibrated','evm_warning':warning,
                    'payload_clipped_fraction':round(clipped,4),'constellation':[[round(float(v.real),4),round(float(v.imag),4)] for v in q[:1024]],
                    'frame_start_sample':first*2+decimation_phase-384, 'end_sample':raw_end, 'ltf_start_sample':first*2+decimation_phase,'ltf_match':round(score,4),'phy':'802.11 legacy OFDM'}
    return None


# Backward compatibility for existing callers; this now supports all rates.
decode_legacy_6mbps = decode_legacy_ofdm
