"""Precision-measurement strategy on the 19 h contiguous stream: window shape/length, and gating by an Im(x)-based vibration indicator."""
import numpy as np, pandas as pd
from scipy import signal
F='../../2026-10-04_contiguous_phasor_capture/data/phasor_stream_20261004_204703.csv'
cols=['cycles','gap_cycles','frame_gap','first_batch','iB','qB','iA','qA','iS1','qS1','iS2','qS2']
d=pd.read_csv(F,usecols=cols,low_memory=False).dropna()
RB=20833.3333/8/64
t_h=d.cycles.values/2604.1667/3600; T0=20+47/60; tod=(T0+t_h)%24; night=(tod>=23)|(tod<7)
D=(d.iA.values+1j*d.qA.values)-(d.iB.values+1j*d.qB.values)
rot=lambda dd: np.exp(-1j*np.radians(dd))
x1=(d.iS1.values+1j*d.qS1.values)/480/D*rot(-6.0); x2=(d.iS2.values+1j*d.qS2.values)/480/D*rot(-9.15)
y1=2*439*1000*x1.real; y2=2*495*1000*x2.real; r1=x1.imag; r2=x2.imag
brk=(d.gap_cycles.values!=0)|(d.frame_gap.values!=0)|(d.first_batch.values==1); seg=np.cumsum(brk)
segs=[]; st=0
for e in np.where(np.diff(np.r_[seg,-1])!=0)[0]: segs.append((st,e+1)); st=e+1
def windows(N):
    """index arrays (start,end) of back-to-back windows of N batches, per segment, with day/night flag"""
    out=[]
    for a,b in segs:
        k=(b-a)//N
        for i in range(k): out.append((a+i*N,a+(i+1)*N))
    return out
def wfun(kind,N):
    if kind=='box': w=np.ones(N)
    elif kind=='tri': w=signal.windows.triang(N)
    elif kind=='hann': w=signal.windows.hann(N+2)[1:-1]
    return w/w.sum()
def est(y,W,w):  # windows (start,end) arrays
    return np.array([y[s:e]@w for s,e in W])
def adjacent_scatter(e,mask):
    ok=mask[1:]&mask[:-1]; dd=np.diff(e)[ok]; return np.sqrt(0.5*np.mean(dd**2)),ok.sum()
print('repeatability of precision-window estimates (adjacent back-to-back windows), um nominal, no gating')
print('%-16s %7s | %s'%('window','length',' | '.join('%s %s'%(a,b) for a in ('S1','S2','S1-S2') for b in ('night','day'))))
res={}
for T in (2.0,3.0,5.0):
    N=int(round(T*RB)); W=windows(N); mid=np.array([(s+e)//2 for s,e in W]); nm=night[mid]
    for kind in ('box','tri','hann'):
        w=wfun(kind,N); e1=est(y1,W,w); e2=est(y2,W,w); ed=e1-e2
        row=[]
        for e in (e1,e2,ed):
            row+= [adjacent_scatter(e,nm)[0],adjacent_scatter(e,~nm)[0]]
        res[(T,kind)]=row
        print('%-16s %6.1fs | %s'%(kind+' %d'%N,T,' | '.join('%.4f'%v for v in row)))
# ---- gating by an Im(x)-based indicator
print('\nGating: vibration indicator from Im(x) (rms batch-to-batch step of the residual inside the window) vs Re-based truth')
N=int(round(2.0*RB)); W=windows(N); mid=np.array([(s+e)//2 for s,e in W]); nm=night[mid]; w=wfun('hann',N)
e1=est(y1,W,w); e2=est(y2,W,w); ed=e1-e2
def ind(r): return np.array([np.sqrt(np.mean(np.diff(r[s:e])**2)) for s,e in W])
def ind_re(y): return np.array([np.sqrt(np.mean((y[s:e-2]-2*y[s+1:e-1]+y[s+2:e])**2)) for s,e in W])
iIm=np.maximum(ind(r1),ind(r2)); iRe=np.maximum(ind_re(y1),ind_re(y2))
print('corr(log Im-indicator, log Re 2nd-difference indicator) = %.2f'%np.corrcoef(np.log(iIm),np.log(iRe))[0,1])
# error proxy: |difference to the neighbouring window| of the differential estimate
err=np.abs(np.diff(ed)); 
for q,lab in ((0.5,'above median'),(0.9,'above 90th pct')):
    th=np.quantile(iIm,q); hi=iIm[:-1]>th
    print('  windows with Im-indicator %s: mean |step to next window| %.4f vs %.4f for the rest (S1-S2)'%(lab,err[hi].mean(),err[~hi].mean()))
print('\nrestart-if-flagged strategy (2 s Hann window, retry up to 2 more times within 5 s total); threshold = percentile of the Im indicator over the whole run')
for pct in (80,90,95):
    th=np.percentile(iIm,pct); good=iIm<=th
    for per,mk in (('night',nm),('day',~nm)):
        p_ok=good[mk].mean()
        # scatter between accepted adjacent windows, expected number of 2 s tries before success within 3 tries
        a_ok=adjacent_scatter(ed,good&mk)[0]
        a_all=adjacent_scatter(ed,mk)[0]
        tries=(1-(1-p_ok)**3); exp_t=(1+(1-p_ok)+(1-p_ok)**2)
        print('  thr p%d %-5s: accepted %.0f%% | scatter accepted %.4f vs all %.4f | P(success within 3 tries=6 s) %.3f, mean tries %.2f'%(pct,per,100*p_ok,a_ok,a_all,tries,exp_t))

print('\nWhich indicator gates best? 2 s Hann windows, accept the lowest-indicator 85%; scatter between adjacent accepted windows (S1-S2, um)')
def rank(x): return np.argsort(np.argsort(x))/len(x)
cands={'Im residual step (firmware-style)':iIm,'Re 2nd difference (pendulum/Nyquist)':iRe,'Im + Re combined (max of ranks)':np.maximum(rank(iIm),rank(iRe)),
       'Re window std (detrended)':np.array([np.std(y1[s:e]-y2[s:e]) for s,e in W])}
for nm_,v in cands.items():
    th=np.quantile(v,0.85); good=v<=th
    print('  %-40s night %.4f (all %.4f) | day %.4f (all %.4f)'%(nm_,adjacent_scatter(ed,good&nm)[0],adjacent_scatter(ed,nm)[0],adjacent_scatter(ed,good&~nm)[0],adjacent_scatter(ed,~nm)[0]))
# how long do disturbances last? run lengths of consecutive flagged windows (p90 of Im indicator)
th=np.percentile(iIm,90); bad=iIm>th
runs=np.diff(np.flatnonzero(np.diff(np.r_[0,bad.astype(int),0])!=0))[::2]
print('\nflagged-run lengths (in 2 s windows, p90 threshold): median %d, p90 %d, max %d; %.1f%% of runs longer than 1 window'%(np.median(runs),np.percentile(runs,90),runs.max(),100*np.mean(runs>1)))

print('\nRobust view (S1-S2, 2 s Hann): MAD-sigma and 95th/99th percentile of |adjacent difference| -- all vs gated(Im, p90)')
good=iIm<=np.percentile(iIm,90)
for per,mk in (('night',nm),('day',~nm)):
    for lab,m in (('all',mk),('gated',good&mk)):
        ok=m[1:]&m[:-1]; dd=np.diff(ed)[ok]
        print('  %-5s %-6s n=%6d  rms/sqrt2 %.4f  MADsigma/sqrt2 %.4f  p95 %.4f  p99 %.4f  max %.3f'%(per,lab,ok.sum(),np.sqrt(0.5*np.mean(dd**2)),1.4826*np.median(np.abs(dd-np.median(dd)))/np.sqrt(2),np.percentile(np.abs(dd),95),np.percentile(np.abs(dd),99),np.abs(dd).max()))

nmed=np.median(iIm[nm]); print('\nIm indicator: night median %.3e, night p99 %.1fx, day median %.1fx, p90(all) = %.1fx, p99(all) %.1fx of the night median'%(nmed,np.percentile(iIm[nm],99)/nmed,np.median(iIm[~nm])/nmed,np.percentile(iIm,90)/nmed,np.percentile(iIm,99)/nmed))
