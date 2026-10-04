"""Averaged power spectrum of the RAW ADC samples (all 4 channels), night vs day."""
import numpy as np, load
from scipy import signal
t,raw=load.load_raw(); th=t/3600
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0)
x=raw[:,8:,:].astype(np.float64)       # drop cycle 0
N=x.shape[1]; FS=20833.333; w=signal.windows.hann(N); f=np.fft.rfftfreq(N,1/FS)
acc={('night',c):np.zeros(len(f)) for c in range(4)}; acc.update({('day',c):np.zeros(len(f)) for c in range(4)})
for i in range(len(t)):
    key='night' if night[i] else 'day'
    X=np.fft.rfft((x[i]-x[i].mean(0))*w[:,None],axis=0)
    for c in range(4): acc[(key,c)]+=np.abs(X[:,c])**2
nn=night.sum();nd=(~night).sum()
P={k:v/(nn if k[0]=='night' else nd) for k,v in acc.items()}
np.savez('rawspec.npz',f=f,**{f'{k[0]}_{k[1]}':v for k,v in P.items()})
print('N',N,'bin Hz',f[1])
carrier=767
for c,nm in enumerate(['S2(ch0)','B(ch1)','A(ch2)','S1(ch3)']):
    for k in ('night','day'):
        p=P[(k,c)]; pc=p[carrier-1:carrier+2].sum()
        print(nm,k,'carrier bin power -> amplitude (counts peak) %.0f'%(np.sqrt(pc*2)/ (w.sum()/2)))
