"""Do robust estimators help when batch values are logged decimated (as in the 20 Hz stream) but hurt when contiguous?"""
import numpy as np, load
from scipy import stats
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]; D=A-B
n=64; nb=K//n
def batches(S,d0):
    Sb=S[:,:nb*n].reshape(R,nb,n).sum(2); Db=D[:,:nb*n].reshape(R,nb,n).sum(2)
    x=Sb/480/Db; return 2*d0*1000*x.real, x.imag
b1,r1=batches(S1,439); b2,r2=batches(S2,495)
rng=np.random.default_rng(1)
def st(x): return np.std(np.diff(x))/np.sqrt(2)
# decimation pattern like the stream: pick batches with gaps of 2 or 3 (mean 2.33), random start
def decim_idx(): 
    idx=[];k=rng.integers(0,3)
    while k<nb: idx.append(k); k+=rng.choice([2,2,3])
    return np.array(idx)
pats=[decim_idx() for _ in range(R)]
def flagsub(res,idx):
    s=np.abs(np.diff(res[idx])); base=np.mean(s)           # per-capture baseline (no warm-up possible here)
    good=np.ones(len(idx),bool); good[1:]=s<=3*base; return good
def ests(b,res,which):
    out=np.zeros(R)
    for i in range(R):
        if which=='contig mean (all 11)': out[i]=b[i].mean()
        else:
            idx=pats[i]; v=b[i][idx]
            if which=='decim mean': out[i]=v.mean()
            elif which=='decim median': out[i]=np.median(v)
            elif which=='decim trim25': out[i]=stats.trim_mean(v,.25)
            elif which=='decim flag-filtered': out[i]=v[flagsub(res[i],idx)].mean()
    return out
print('30 s scatter of capture estimate (um nominal): night S1 S2 S1-S2 | day S1 S2 S1-S2')
for w in ('contig mean (all 11)','decim mean','decim median','decim trim25','decim flag-filtered'):
    e1=ests(b1,r1,w);e2=ests(b2,r2,w); r=[]
    for m in (night,day): r+=[st(e1[m]),st(e2[m]),st((e1-e2)[m])]
    print('  %-24s'%w,' '.join('%.3f'%v for v in r[:3]),'|',' '.join('%.3f'%v for v in r[3:]))
