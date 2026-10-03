import numpy as np, load
from scipy import signal
exec(open('strategies.py').read().split("print('--- (1)")[0])
# (a) selection-bias check for the quality flag
n=64;m=K//n; sums=lambda X:X[:,:m*n].reshape(R,m,n).sum(2)
Db=sums(A)-sums(B); xb=sums(S1)/480/Db; b=2*439*1000*xb.real; res=xb.imag
good=np.ones(b.shape,bool); base=None
for r in range(R):
    for k in range(1,m):
        s=abs(res[r,k]-res[r,k-1])
        if base is None: base=s; continue
        g=s<=3*base; good[r,k]=g
        if g: base+=(s-base)/32
dev=b-b.mean(1,keepdims=True)
print('flagged batches: rms dev from capture mean %.3f um, mean(signed) %.4f ; kept: rms %.3f'%(np.sqrt((dev[~good]**2).mean()),dev[~good].mean(),np.sqrt((dev[good]**2).mean())))
# what do the kept batches look like on a capture with flagged ones: variance of kept-mean vs full-mean error relative to a smooth 'truth' (Hann mean)
truth=((y1*signal.windows.hann(K)).sum(1)/signal.windows.hann(K).sum())
e_full=b.mean(1)-truth; w=good.astype(float); e_kept=(b*w).sum(1)/w.sum(1)-truth
print('error vs Hann-mean truth: full-mean rms %.3f ; kept-mean rms %.3f um'%(e_full.std(),e_kept.std()))
# (b) triangular window (= two cascaded boxcars)
tri=signal.windows.triang(K)
for nm,wv in (('rect',np.ones(K)),('triangular',tri),('Hann',signal.windows.hann(K))):
    r=[]
    for mm in (night,day):
        for y in (y1,y2,y1-y2): r.append(st(((y*wv).sum(1)/wv.sum())[mm]))
    print('  %-10s'%nm,' '.join('%6.3f'%x for x in r[:3]),'|',' '.join('%6.3f'%x for x in r[3:]))
# (c) differential cancellation of the pendulum band
Zs=lambda y:np.fft.rfft((y-y.mean(1,keepdims=True))*signal.windows.hann(K),8192,axis=1)
fr=np.fft.rfftfreq(8192,1/FS); bd=(fr>15)&(fr<26)
for nm,mm in (('night',night),('day',day)):
    Z1=Zs(y1[mm]);Z2=Zs(y2[mm])
    p1=(np.abs(Z1[:,bd])**2).sum();p2=(np.abs(Z2[:,bd])**2).sum()
    gopt=(np.real(np.conj(Z2[:,bd])*Z1[:,bd]).sum())/p2
    for g in (1.0,gopt):
        pd=(np.abs(Z1[:,bd]-g*Z2[:,bd])**2).sum()
        print('  %s: S1-g*S2 pendulum-band power / S1 : g=%.3f -> %.1f dB'%(nm,g,10*np.log10(pd/p1)))
    print('   S2/S1 pendulum amplitude ratio %.2f'%np.sqrt(p2/p1))
# (d) Nyquist observation: batch stream spectrum fold
print('batch rate %.2f Hz, Nyquist %.2f Hz; pendulum 19.9 Hz sits %.2f Hz below Nyquist; 64-cycle boxcar passes it at %.1f dB'%(FS/64,FS/128,FS/128-19.9,20*np.log10(abs(np.sin(np.pi*19.9*64/FS)/(64*np.sin(np.pi*19.9/FS))))))
