import numpy as np, pandas as pd
from scipy.signal import lombscargle, find_peaks
F='../../2026-09-27_24hr_granite_plate_test/data/granite_24hr.csv'
d=pd.read_csv(F,usecols=['t_s','delta1_mm_raw','delta2_mm_raw','iB','qB','iA','qA'],low_memory=False).dropna()
t=d.t_s.values; tm=t/60
A=d.iA.values+1j*d.qA.values;B=d.iB.values+1j*d.qB.values; zre=(-(A+B)/(2*(A-B))).real*1e6
s1=d.delta1_mm_raw.values*1e3-1.419*zre; s2=d.delta2_mm_raw.values*1e3-1.424*zre
for nm,(m0,m1) in (('quiet 200-245 min',(200,245)),('operator present 0-180 min',(0,180))):
    sel=(tm>=m0)&(tm<m1)
    ts=t[sel]; 
    # take a 10-minute sub-window to keep Lomb-Scargle tractable
    ts=ts[:int(10*60*24)]; 
    fr=np.linspace(0.05,12,2400)
    for lab,x in (('S1',s1[sel][:len(ts)]),('S2',s2[sel][:len(ts)]),('S1-S2',(s1-s2)[sel][:len(ts)])):
        x=x-x.mean(); p=lombscargle(ts,x,2*np.pi*fr,normalize=False)*2/len(ts)    # ~ amplitude^2 per bin
        pk,_=find_peaks(p,height=p.max()*0.05)
        top=sorted(pk,key=lambda i:-p[i])[:6]
        low=p[fr<1].sum(); mid=p[(fr>=1)&(fr<3)].sum(); hi=p[fr>=3].sum(); tot=low+mid+hi
        print('%-28s %-6s peaks: %s | power share <1 Hz %.0f%%, 1-3 Hz %.0f%%, >3 Hz %.0f%%'%(nm,lab,' '.join('%.2fHz'%fr[i] for i in top),100*low/tot,100*mid/tot,100*hi/tot))
