"""How much does a 2-3 s precision measurement buy? Uses the continuous 24 h granite-plate stream (fw 0.10.53)."""
import numpy as np, pandas as pd, os
F='../../2026-09-27_24hr_granite_plate_test/data/granite_24hr.csv'
cols=['t_s','delta1_mm_raw','residual1','delta2_mm_raw','residual2','quality1_ok','quality2_ok','iB','qB','iA','qA','charging']
d=pd.read_csv(F,usecols=cols,low_memory=False)
d=d.dropna(subset=['iA','iB','delta1_mm_raw']); d=d[d.t_s.diff().fillna(1)>=0]
t=d.t_s.values; A=d.iA.values+1j*d.qA.values; B=d.iB.values+1j*d.qB.values
zre=(-(A+B)/(2*(A-B))).real*1e6
s1=d.delta1_mm_raw.values*1e3; s2=d.delta2_mm_raw.values*1e3
C1,C2=1.419,1.424
S={'S1':s1-C1*zre,'S2':s2-C2*zre}; S['S1-S2']=S['S1']-S['S2']
S_unc={'S1':s1,'S2':s2,'S1-S2':s1-s2}
q1=d.quality1_ok.values.astype(bool); q2=d.quality2_ok.values.astype(bool)
tm=t/60
print('rows',len(t),'median dt %.4f s -> %.1f Hz logged'%(np.median(np.diff(t)),1/np.median(np.diff(t))))
segs={'operator present (0-180 min)':(0,180),'quiet (200-245 min)':(200,245),'quiet (340-1050 min)':(340,1050)}
def adev_bins(x,tt,T,m0,m1,est=np.mean,minfrac=.6):
    sel=(tm>=m0)&(tm<m1); xs=x[sel]; ts=tt[sel]
    b=np.floor((ts-ts[0])/T).astype(int); nb=b.max()+1
    cnt=np.bincount(b,minlength=nb)
    if est is np.mean:
        mean=np.bincount(b,xs,minlength=nb)/np.maximum(cnt,1)
    else:
        order=np.argsort(b,kind='stable'); xs2=xs[order]; edges=np.r_[0,np.cumsum(cnt)]
        mean=np.array([est(xs2[edges[i]:edges[i+1]]) if cnt[i]>0 else np.nan for i in range(nb)])
    ok=cnt>=minfrac*np.median(cnt[cnt>0]); mean=np.where(ok,mean,np.nan)
    dd=np.diff(mean); dd=dd[~np.isnan(dd)]
    return np.sqrt(0.5*np.mean(dd**2)), len(dd)
Ts=[0.2,0.5,1.0,1.6,2.0,3.0,4.0,6.0,10.0,30.0]
for sn,(m0,m1) in segs.items():
    print('\n=== %s : repeatability (ADEV of back-to-back window means), um, z-corrected'%sn)
    print('  T(s)    '+'  '.join('%7s'%k for k in S))
    for T in Ts:
        print('  %5.1f   '%T+'  '.join('%7.3f'%adev_bins(S[k],t,T,m0,m1)[0] for k in S))
print('\nSame, uncorrected (with the old A/B-balance term, S1-S2 unaffected), quiet 340-1050:')
for T in (0.2,2.0,3.0):
    print('  %5.1f   '%T+'  '.join('%7.3f'%adev_bins(S_unc[k],t,T,340,1050)[0] for k in S_unc))
print('\n=== strategy at T=2.5 s (um): mean | median | trim10 | trim25 | flag-filtered mean')
from scipy import stats
def flagmean(q):
    def f(x): return x.mean()
    return f
for sn,(m0,m1) in segs.items():
    for k in ('S1','S2','S1-S2'):
        x=S[k]
        r=[adev_bins(x,t,2.5,m0,m1,est=np.mean)[0],adev_bins(x,t,2.5,m0,m1,est=np.median)[0],
           adev_bins(x,t,2.5,m0,m1,est=lambda v:stats.trim_mean(v,.1))[0],adev_bins(x,t,2.5,m0,m1,est=lambda v:stats.trim_mean(v,.25))[0]]
        # flag-filtered: set bad samples to nan then nanmean
        if k=='S1': xf=np.where(q1,x,np.nan)
        elif k=='S2': xf=np.where(q2,x,np.nan)
        else: xf=np.where(q1&q2,x,np.nan)
        r.append(adev_bins(xf,t,2.5,m0,m1,est=np.nanmean)[0])
        print('  %-30s %-6s '%(sn,k)+' | '.join('%.3f'%v for v in r))
np.savez('prec_window.npz',Ts=Ts)
