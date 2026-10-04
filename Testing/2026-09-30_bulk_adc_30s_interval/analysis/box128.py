import numpy as np, load
from scipy import signal
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600; FS=20833.333/8
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]; D=A-B
y1=2*439*1000*(S1/480/D).real; y2=2*495*1000*(S2/480/D).real
def box(L,f): return np.abs(np.sin(np.pi*f*L/FS)/(L*np.sin(np.pi*f/FS)))
print('pure-tone gain (dB) of one boxcar of L cycles vs pendulum frequency')
fl=[17,18,19,19.5,19.9,20.35,21,21.5,22,23]
print('  f(Hz)   ',' '.join('%6.1f'%f for f in fl))
for L in (64,128,131,256,512):
    print('  L=%3d   '%L,' '.join('%6.1f'%(20*np.log10(box(L,f)+1e-9)) for f in fl))
print('  tri 2x480',' '.join('%6.1f'%(40*np.log10(box(480,f)+1e-9)) for f in fl))
def adev_batches(y,n,m):
    nb=K//n; z=y[m][:,:nb*n].reshape(m.sum(),nb,n).mean(2)
    return np.sqrt(0.5*np.mean(np.diff(z,axis=1)**2))
print('\nbatch-to-batch jitter (Allan-style, um) night | day, S1 S2')
for n in (64,96,128,131,192,256):
    print('  n=%3d (%5.1f ms) '%(n,n*8/20.8333),' '.join('%.3f'%adev_batches(y,n,m) for m in (night,day) for y in (y1,y2)))
# pendulum frequency of each capture
w=signal.windows.hann(K); Z=np.fft.rfft((y1-y1.mean(1,keepdims=True))*w,16384,axis=1); f=np.fft.rfftfreq(16384,1/FS)
band=(f>12)&(f<30); pf=f[band][np.argmax(np.abs(Z[:,band]),1)]
snr=(np.abs(Z[:,band])**2).max(1)/np.median(np.abs(Z[:,(f>200)&(f<1000)])**2,1); ok=snr>100
print('\njitter vs per-capture pendulum frequency (day+night, only strong-pendulum captures), S2 um: 64 | 128 | 131 | triangular-weighted 640')
def tri_est(y,N=640):
    wv=signal.windows.triang(N);return (y[:,:N]*wv).sum(1)/wv.sum()
for lo,hi in ((15,18.5),(18.5,19.5),(19.5,20.5),(20.5,21.5),(21.5,26)):
    m=ok&(pf>=lo)&(pf<hi)
    if m.sum()<30: continue
    def aj(y,n):
        nb=K//n; z=y[m][:,:nb*n].reshape(m.sum(),nb,n).mean(2); return np.sqrt(0.5*np.mean(np.diff(z,axis=1)**2))
    print('  %4.1f-%4.1f Hz n=%3d  %.3f | %.3f | %.3f'%(lo,hi,m.sum(),aj(y2,64),aj(y2,128),aj(y2,131)))
# capture-level estimator, exact multiples
def st(x): return np.std(np.diff(x))/np.sqrt(2)
print('\n30 s scatter of capture estimate (um) night S1 S2 S1-S2 | day S1 S2 S1-S2')
for nm,fn in (('mean 704 (11x64)',lambda y:y[:,:704].mean(1)),('mean 640 (5x128)',lambda y:y[:,:640].mean(1)),('mean 512 (2x256 / 8x64)',lambda y:y[:,:512].mean(1)),('mean 262 (2 periods)',lambda y:y[:,:262].mean(1)),('triangular 640',tri_est)):
    r=[]
    for m in (night,day):
        a=fn(y1)[m];b=fn(y2)[m]; r+=[st(a),st(b),st(fn(y1)[m]-fn(y2)[m])]
    print('  %-26s'%nm,' '.join('%.3f'%v for v in r[:3]),'|',' '.join('%.3f'%v for v in r[3:]))
