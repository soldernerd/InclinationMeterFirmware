import numpy as np
from tune_quality import *
q1,q2=q_of(r1),q_of(r2)
def min_floor(m,eps):
    f=np.full(n,np.nan); fl=np.nan
    for k in range(n):
        v=m[k]
        if np.isnan(v): continue
        fl = v if np.isnan(fl) else min(fl*(1+eps), v)
        f[k]=fl
    return f
W=81
m1=rolling_mean(q1,W); m2=rolling_mean(q2,W)
for eps_t,lab in ((20,'x2 per 20 min'),(60,'x2 per 60 min')):
    eps=np.log(2)/(eps_t*60*RB)
    f1=min_floor(m1,eps); f2=min_floor(m2,eps)
    R=np.fmax(m1/f1,m2/f2); ok=~np.isnan(R)
    print('\nmin-follower floor, creep %s: ratio window-mean-q/floor (81 batches), max over S1,S2'%lab)
    print('  night pct 50/90/99/99.9: %s'%[round(np.nanpercentile(R[night&ok],p),2) for p in (50,90,99,99.9)])
    print('  day   pct 50/90/99/99.9: %s'%[round(np.nanpercentile(R[~night&ok],p),2) for p in (50,90,99,99.9)])
    for K in (1.5,1.75,2.0,2.5,3.0):
        print('  K=%.2f: rejected night %.1f%%  day %.1f%%'%(K,100*np.mean(R[night&ok]>K),100*np.mean(R[~night&ok]>K)))
    # display window 25 vs the same floor
    m25_1=rolling_mean(q1,25); m25_2=rolling_mean(q2,25)
    R25=np.fmax(m25_1/f1,m25_2/f2); ok25=~np.isnan(R25)
    print(' display (25-batch window) ratio to the same floor: night 50/90/99: %s | day: %s'%([round(np.nanpercentile(R25[night&ok25],p),2) for p in (50,90,99)],[round(np.nanpercentile(R25[~night&ok25],p),2) for p in (50,90,99)]))
    for K in (2.0,2.5,3.0,4.0):
        print('  K_disp=%.1f: flagged night %.1f%%  day %.1f%%'%(K,100*np.mean(R25[night&ok25]>K),100*np.mean(R25[~night&ok25]>K)))
np.savez('quality_tune.npz',f1=f1,f2=f2)
