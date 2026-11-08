#!/usr/bin/env python3
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from pathlib import Path
root=Path('../multicast-frame-experiment2')
x=np.fromfile(root/'candidate-1-capture-2747.cf32',dtype='<c8');rate=40e6
lo,hi=4700,6400

def similarity(lag,window):
 a=x[:-lag];b=x[lag:]
 def sums(v):
  s=np.r_[0,np.cumsum(v,dtype=np.complex128 if np.iscomplexobj(v) else np.float64)];return s[window:]-s[:-window]
 return abs(sums(a*b.conj()))**2/(sums(abs(a)**2)*sums(abs(b)**2)+1e-20)
q32=similarity(32,128);q128=similarity(128,128)
t=(np.arange(lo,hi)-5200)/rate*1e6
fig,ax=plt.subplots(2,1,figsize=(10,5.8),sharex=True)
ax[0].plot(t,abs(x[lo:hi])**2,lw=.65,color='#176b87');ax[0].set_ylabel('IQ power (relative)');ax[0].grid(alpha=.25)
ax[1].plot(t,q32[lo:hi],label='32-sample repetition',color='#df6b26');ax[1].plot(t,q128[lo:hi],label='128-sample repetition',color='#7741a6',alpha=.8);ax[1].set_ylim(0,1.05);ax[1].set_ylabel('Normalized similarity');ax[1].set_xlabel('Time relative to selected sample 5200 (µs)');ax[1].legend();ax[1].grid(alpha=.25)
fig.suptitle('Multicast IQ candidate — burst 2747, 40 MS/s\nRepetition is consistent with OFDM training; frame identity is not confirmed',fontsize=11)
fig.tight_layout();fig.savefig(root/'candidate-1-repetition.png',dpi=150)
