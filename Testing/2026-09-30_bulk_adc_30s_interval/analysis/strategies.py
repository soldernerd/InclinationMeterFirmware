"""Test strategies against the ~20 Hz pendulum band + the firmware's quality flag, on the real 24 h data."""
import numpy as np, load, stats_core as sc
from scipy import signal
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
D=A-B; FS=20833.333/8
y1=2*439*1000*(S1/480/D).real; y2=2*495*1000*(S2/480/D).real      # um, nominal, per cycle
def st(x): return np.std(np.diff(x))/np.sqrt(2)
def row(f):
    out=[]
    for m in (night,day):
        for y in (y1,y2,y1-y2): out.append(st(f(y)[m]))
    return out
def show(nm,f,base=None):
    r=row(f); print('  %-44s'%nm,' '.join('%6.3f'%v for v in r[:3]),'|',' '.join('%6.3f'%v for v in r[3:]),
      ('  rel.mean: '+' '.join('%.2f'%(a/b) for a,b in zip(r,base))) if base else ''); return r
print('--- (1) firmware quality flag, continuous-EWMA emulation (64-cycle batches, 11/capture, EWMA carried across captures)')
n=64;m=K//n; sums=lambda X:X[:,:m*n].reshape(R,m,n).sum(2)
Db=sums(A)-sums(B); xb1=sums(S1)/480/Db; xb2=sums(S2)/480/Db
def flags(res):
    good=np.ones(res.shape,bool); base=None
    for r in range(R):
        for k in range(1,m):
            s=abs(res[r,k]-res[r,k-1])
            if base is None: base=s; continue
            g=s<=3*base; good[r,k]=g
            if g: base+=(s-base)/32
    return good
g1=flags(xb1.imag);g2=flags(xb2.imag)
print('  flagged: night S1 %.1f%% S2 %.1f%% | day S1 %.1f%% S2 %.1f%%  (Gaussian steps would flag ~1.6%%)'%(100*(1-g1[night].mean()),100*(1-g2[night].mean()),100*(1-g1[day].mean()),100*(1-g2[day].mean())))
b1=2*439*1000*xb1.real; b2=2*495*1000*xb2.real
def gmean(b,g): w=g.astype(float);return (b*w).sum(1)/np.maximum(w.sum(1),1)
base=row(lambda y:y.mean(1)) if False else None
print('  (30 s scatter, um)  columns: night S1 S2 S1-S2 | day S1 S2 S1-S2')
a=[];
for nm,fn in (('mean of 11 batches',lambda:(b1.mean(1),b2.mean(1))),('flag-filtered mean',lambda:(gmean(b1,g1),gmean(b2,g2)))):
    u,v=fn(); r=[]
    for mm in (night,day): r+= [st(u[mm]),st(v[mm]),st((u-v)[mm])]
    print('  %-44s'%nm,' '.join('%6.3f'%x for x in r[:3]),'|',' '.join('%6.3f'%x for x in r[3:]))
# does the flag relate to vibration? flagged fraction vs 20 Hz band power
w=signal.windows.hann(K); Z=np.fft.rfft((y1-y1.mean(1,keepdims=True))*w,8192,axis=1); fr=np.fft.rfftfreq(8192,1/FS)
pb=(np.abs(Z[:,(fr>15)&(fr<26)])**2).sum(1)
q=np.quantile(pb,[.33,.66]); lv=np.digitize(pb,q)
print('  flag rate by 20 Hz-band power tercile (S1):',[round(100*(1-g1[lv==i].mean()),1) for i in range(3)])
print('  30 s scatter of 11-batch mean by vibration tercile (S1, um):',[round(st(b1.mean(1)[lv==i]),3) for i in range(3)], '(subsequence diff, approximate)')
print('--- (2) window / notch strategies on the 767-cycle capture (per-cycle y; um)')
Tp=FS/19.9  # cycles per pendulum period
base=show('rect mean, 704 cycles (11 batches, as now)',lambda y:y[:,:704].mean(1))
show('rect mean, all 767',lambda y:y.mean(1),base)
for kp in (3,4,5):
    L=int(round(kp*Tp)); show('rect mean, %d periods = %d cyc (%.0f ms)'%(kp,L,L*8/20.8333),lambda y,L=L:y[:,:L].mean(1),base)
for nm,wf in (('Hann',signal.windows.hann),('Blackman',signal.windows.blackman),('Tukey .5',lambda N:signal.windows.tukey(N,.5))):
    show('%s window, 767'%nm,lambda y,wf=wf:(y*wf(K)).sum(1)/wf(K).sum(),base)
# regression notch: DC + sin/cos on a frequency grid
def reg_notch(freqs):
    n=np.arange(K)/FS; cols=[np.ones(K)]
    for f in freqs: cols+= [np.cos(2*np.pi*f*n),np.sin(2*np.pi*f*n)]
    X=np.array(cols).T; pinv=np.linalg.pinv(X)
    return lambda y:(y@pinv.T)[:,0]
for nm,fl in (('regress DC + 20 Hz tone',[19.9]),('regress DC + 15..25 Hz step 2.5',np.arange(15,25.1,2.5)),('regress DC + 12..28 Hz step 2',np.arange(12,28.1,2)),('regress DC + 10..32 Hz step 2',np.arange(10,32.1,2))):
    show(nm,reg_notch(fl),base)
print('--- (3) vibration-gated / inverse-variance combination across the capture (5 segments of ~131 cycles)')
seg=5;L=K//seg
def segstats(y):
    ys=y[:,:L*seg].reshape(R,seg,L); mean=ys.mean(2)
    # segment pendulum energy: band-power from the segment's own demod of 17-23 Hz is crude at 131 samples; use detrended variance as proxy
    var=ys.var(2); return mean,var
mean,var=segstats(y1)
def ivw(y):
    mn,v=segstats(y); w=1/(v+1e-12); return (mn*w).sum(1)/w.sum(1)
show('segment inverse-variance weighted mean',ivw,base)
show('segment median of 5',lambda y:np.median(segstats(y)[0],1),base)
# tone attenuation of current estimator vs frequency
def sinc_att(L,f): return np.abs(np.sin(np.pi*f*L/FS)/(L*np.sin(np.pi*f/FS)))
print('--- (4) pure-tone attenuation of rectangular windows at pendulum freq (dB)')
for f in (17,18,19,19.9,20.35,21,22,24):
    print('  f=%5.2f Hz  64cyc %6.1f | 512cyc(MA8, now) %6.1f | 704cyc %6.1f | 1024cyc %6.1f | 131cyc(1 period) %6.1f'%(f,*[20*np.log10(sinc_att(L,f)+1e-12) for L in (64,512,704,1024,131)]))
