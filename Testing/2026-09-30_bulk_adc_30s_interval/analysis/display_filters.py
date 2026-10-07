"""Compare display-stream window shapes on the real 19 h contiguous phasor stream (fw 0.10.65)."""
import numpy as np, pandas as pd
from scipy import signal
F='../../2026-10-04_contiguous_phasor_capture/data/phasor_stream_20261004_204703.csv'
cols=['cycles','gap_cycles','frame_gap','first_batch','iB','qB','iA','qA','iS1','qS1','iS2','qS2']
d=pd.read_csv(F,usecols=cols,low_memory=False).dropna()
t_h=d.cycles.values/2604.1667/3600
A=d.iA.values+1j*d.qA.values; B=d.iB.values+1j*d.qB.values; D=A-B
S1=d.iS1.values+1j*d.qS1.values; S2=d.iS2.values+1j*d.qS2.values
y1=2*439*1000*(S1/480/D).real; y2=2*495*1000*(S2/480/D).real
brk=(d.gap_cycles.values!=0)|(d.frame_gap.values!=0)|(d.first_batch.values==1)
seg=np.cumsum(brk)
print('rows',len(d),'hours %.1f'%t_h.max(),'segments',seg.max()+1,'breaks',int(brk.sum()))
# run starts 20:47 -> night (23:00-07:00) = t in [2.22,10.22] h
T0=20+47/60
night=((T0+t_h)%24>=23)|((T0+t_h)%24<7)
def c(*st):
    h=np.array([1.0])
    for L in st: h=np.convolve(h,np.ones(L))
    return h
filters=[('boxcar 10 (plain average)',np.ones(10)),('triangular 19 (current)',c(10,10)),('Hann 19',signal.windows.hann(21)[1:-1]),
         ('Blackman 19',signal.windows.blackman(21)[1:-1]),('3 x boxcar (7,7,7)',c(7,7,7)),('Hann 25',signal.windows.hann(27)[1:-1])]
def outputs(y,h,decim=10):
    h=h/h.sum(); outs=[];tt=[]
    ids=np.where(np.diff(np.r_[seg,-1])!=0)[0]; start=0
    for e in ids:
        yy=y[start:e+1]; 
        if len(yy)>len(h)*3:
            z=np.convolve(yy,h[::-1],mode='valid')[::decim]
            outs.append(z); tt.append(t_h[start+len(h)-1:e+1][::decim][:len(z)])
        start=e+1
    return outs,tt
def adev_streams(outs,tt,mask_fn,m=1):
    sq=0.0;n=0
    for z,t in zip(outs,tt):
        mk=mask_fn(t)
        zz=z; 
        if m>1:
            k=len(zz)//m; zz=zz[:k*m].reshape(k,m).mean(1); mk=mk[:k*m:m]
        dd=np.diff(zz); ok=mk[1:]&mk[:-1]; dd=dd[ok]
        sq+=np.sum(dd**2); n+=len(dd)
    return np.sqrt(0.5*sq/max(n,1)), n
print('repeatability of the 4 Hz output stream (um nominal): scatter between consecutive outputs / 4-output (1 s) means')
for nm,h in filters:
    row=[]
    for y,lab in ((y1,'S1'),(y2,'S2'),(y1-y2,'S1-S2')):
        outs,tt=outputs(y,h)
        for per,fn in (('night',lambda t:night_of(t)),('day',lambda t:~night_of(t))):
            pass
    pass
def night_of(t): 
    x=(T0+t)%24; return (x>=23)|(x<7)
for nm,h in filters:
    parts=[]
    for y,lab in ((y1,'S1'),(y2,'S2'),(y1-y2,'S1-S2')):
        outs,tt=outputs(y,h)
        for per,fn in (('night',night_of),('day',lambda t:~night_of(t))):
            a1,_=adev_streams(outs,tt,fn,1); a4,_=adev_streams(outs,tt,fn,4)
            parts.append('%s %-5s %.4f/%.4f'%(lab,per,a1,a4))
    print('%-26s'%nm,' | '.join(parts))
