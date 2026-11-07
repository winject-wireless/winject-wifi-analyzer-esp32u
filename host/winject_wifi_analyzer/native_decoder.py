"""Native Viterbi primitive with a portable, slower reference fallback."""
from __future__ import annotations
import numpy as np
try:
    from ._native import viterbi as _native_viterbi
except ImportError:
    _native_viterbi = None

BACKEND = 'native-cpp' if _native_viterbi else 'python'

def _python_viterbi(coded: np.ndarray, force_zero: bool) -> np.ndarray:
    coded = np.asarray(coded, dtype=np.uint8).reshape(-1)
    if len(coded) % 2 or np.any(coded > 2):
        raise ValueError('coded bits must be 0, 1, or erasure 2, with even length')
    ns=np.arange(64,dtype=np.int32);bit=ns&1;p0=ns>>1;p1=p0|32
    def output(prev):
        reg=(prev<<1)|bit
        return (np.array([int(x).bit_count()&1 for x in reg&0o155]),
                np.array([int(x).bit_count()&1 for x in reg&0o117]))
    a0,a1=output(p0);b0,b1=output(p1)
    metric=np.full(64,1_000_000);metric[0]=0;history=[]
    for i in range(0,len(coded),2):
        c0,c1=coded[i],coded[i+1]
        m0=metric[p0]+((a0!=c0)&(c0!=2)).astype(np.int8)+((a1!=c1)&(c1!=2)).astype(np.int8)
        m1=metric[p1]+((b0!=c0)&(c0!=2)).astype(np.int8)+((b1!=c1)&(c1!=2)).astype(np.int8)
        choose=m1<m0;history.append(choose);metric=np.where(choose,m1,m0)
    state=0 if force_zero else int(np.argmin(metric));out=np.empty(len(history),np.uint8)
    for t in range(len(history)-1,-1,-1):
        out[t]=state&1;state=(state>>1)|(32 if history[t][state] else 0)
    return out

def viterbi(coded: np.ndarray, force_zero: bool) -> np.ndarray:
    coded=np.ascontiguousarray(coded,dtype=np.uint8).reshape(-1)
    if _native_viterbi is None:
        return _python_viterbi(coded, force_zero)
    return np.frombuffer(_native_viterbi(coded, force_zero),dtype=np.uint8)
