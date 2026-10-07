import numpy as np
from tune_quality import *
from tune_quality2 import min_floor
W=81; TMAX=int(5.0*RB)
q1,q2=q_of(r1),q_of(r2)
m1=rolling_mean(q1,W); m2=rolling_mean(q2,W)
eps=np.log(2)/(20*60*RB)
f1=min_floor(m1,eps); f2=min_floor(m2,eps)
R=np.fmax(m1/f1,m2/f2)
hw=signal.windows.hann(W+2)[1:-1]; hw/=hw.sum()
H1=np.full(n,np.nan); H2=np.full(n,np.nan)
H1[W-1:]=np.convolve(y1,hw[::-1],mode='valid'); H2[W-1:]=np.convolve(y2,hw[::-1],mode='valid')
HD=H1-H2
def chain(K):
    res=[]; s=1000; fails=0; tries=0; times=[]
    while s+TMAX<n:
        found=None
        for k in range(s+W-1, s+TMAX):
            if R[k]<=K: found=k; break
        tries+=1
        if found is None:
            fails+=1; res.append((s,np.nan,night[s+TMAX//2])); s+=TMAX
        else:
            res.append((s,HD[found],night[found])); times.append((found-s+1)/RB); s=found+1
    return res,fails,tries,np.array(times)
print('%-8s %8s %8s %9s %9s | %-26s | %-26s'%('K','fail%','mean_s','p95_s','acc/try','night rms/sqrt2 (n)','day rms/sqrt2 (n)'))
for K in (np.inf,12,8,6,5,4,3.5,3):
    res,fails,tries,times=chain(K)
    vals=np.array([r[1] for r in res]); nt=np.array([r[2] for r in res])
    dd=np.diff(vals); ok=~np.isnan(dd); nn=nt[1:]
    out=[]
    for mask in (nn,~nn):
        sel=ok&mask; out.append('%.4f (%d)'%(np.sqrt(0.5*np.mean(dd[sel]**2)),sel.sum()))
    print('%-8s %7.1f%% %8.2f %9.2f %9.2f | %-26s | %-26s'%(('none' if K==np.inf else '%g'%K),100*fails/tries,times.mean(),np.percentile(times,95),len(times)/tries,out[0],out[1]))
