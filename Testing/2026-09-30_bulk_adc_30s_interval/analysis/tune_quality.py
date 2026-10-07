"""Tune/emulate the window-level quality indicator (rms step of Im(x)) with a causal floor tracker, on the 19 h contiguous stream."""
import numpy as np, pandas as pd, sys
from scipy import signal
F='../../2026-10-04_contiguous_phasor_capture/data/phasor_stream_20261004_204703.csv'
cols=['cycles','gap_cycles','frame_gap','first_batch','iB','qB','iA','qA','iS1','qS1','iS2','qS2']
d=pd.read_csv(F,usecols=cols,low_memory=False).dropna()
RB=20833.3333/8/64
t_h=d.cycles.values/2604.1667/3600; T0=20+47/60; tod=(T0+t_h)%24; night=(tod>=23)|(tod<7)
D=(d.iA.values+1j*d.qA.values)-(d.iB.values+1j*d.qB.values)
rot=lambda dd: np.exp(-1j*np.radians(dd))
x1=(d.iS1.values+1j*d.qS1.values)/D*rot(-6.0); x2=(d.iS2.values+1j*d.qS2.values)/D*rot(-9.15)
y1=2*439*1000*x1.real/480; y2=2*495*1000*x2.real/480; r1=x1.imag; r2=x2.imag
brk=(d.gap_cycles.values!=0)|(d.frame_gap.values!=0)|(d.first_batch.values==1); seg=np.cumsum(brk)
# only the long first segment is used for emulation (gap-free)
main=np.argmax(np.bincount(seg)); sl=np.where(seg==main)[0]; sl=slice(sl[0],sl[-1]+1)
y1,y2,r1,r2,night=y1[sl],y2[sl],r1[sl],r2[sl],night[sl]
n=len(y1); print('emulating on',n,'contiguous batches (%.1f h)'%(n/RB/3600))
def q_of(r):
    q=np.zeros(n); q[1:]=np.diff(r)**2; return q
def floor_track(q,N0=256,clip=4.0,a_up=1/65536,a_dn=1/2048):
    f=np.zeros(n); fl=0.0
    for k in range(n):
        v=q[k]
        if k<16: fl=(fl*k+v)/(k+1)           # bootstrap: plain mean
        elif k<N0:
            vv=min(v,clip*fl) ; fl=(fl*k+vv)/(k+1)
        else:
            vv=min(v,clip*fl)
            fl+= (a_up if vv>fl else a_dn)*(vv-fl)
        f[k]=fl
    return f
def rolling_mean(q,w):
    c=np.cumsum(np.r_[0,q]); m=np.full(n,np.nan); m[w-1:]=(c[w:]-c[:-w])/w; return m
if __name__=='__main__':
    q1,q2=q_of(r1),q_of(r2)
    for clip,aup in ((4.0,1/65536),(6.0,1/65536),(4.0,1/16384)):
        f1=floor_track(q1,clip=clip,a_up=aup); f2=floor_track(q2,clip=clip,a_up=aup)
        print('\nclip',clip,'a_up 1/%d'%round(1/aup),' floor ratio day/night median (S1): %.2f'%(np.median(f1[~night])/np.median(f1[night])))
        for w,name in ((25,'display 25'),(81,'precision 81')):
            m1=rolling_mean(q1,w)/f1; m2=rolling_mean(q2,w)/f2; mm=np.fmax(m1,m2)
            ok=~np.isnan(mm)
            print(' %s: ratio mean-q/floor percentiles night: p50 %.2f p90 %.2f p99 %.2f | day: p50 %.2f p90 %.2f p99 %.2f'%(name,*[np.nanpercentile(mm[night&ok],p) for p in (50,90,99)],*[np.nanpercentile(mm[~night&ok],p) for p in (50,90,99)]))
