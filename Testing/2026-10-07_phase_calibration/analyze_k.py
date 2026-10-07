"""k calibration from tilt steps: python analyze_k.py <state_name>=<csv> ...   (states in capture order)
Steps = differences between consecutive states; true tilt = shim / base length (S1 200 mm, S2 150 mm).
Shim state per capture is given by the name prefix: front / back / flat  (front=+0.1 mm at the front,
back=+0.1 mm at the back, flat=none). Reading model: inv*Re[u e^{-j d}]/(PGA k0), u = S/(A-B)."""
import csv, sys, numpy as np
DL={'S1':np.deg2rad(-7.17),'S2':np.deg2rad(-9.19)}; PGA=16; K0=0.0213; L={'S1':0.200,'S2':0.150}
POS={'back':-0.1,'flat':0.0,'front':+0.1}   # shim position, mm, positive = front up
def load(fn):
    r=list(csv.DictReader(open(fn))); f=lambda k: np.array([float(x[k]) for x in r])[1:]
    D=(f('iA')+1j*f('qA'))-(f('iB')+1j*f('qB'))
    return {n:(f(i)+1j*f(q))/D for n,(i,q) in (('S1',('iS1','qS1')),('S2',('iS2','qS2')))}, f('gap_cycles').sum()
def reading(u,n): return -np.real(u.mean()*np.exp(-1j*DL[n]))/(PGA*K0)
states=[(a.split('=')[0],*load(a.split('=')[1])) for a in sys.argv[1:]]
print('gaps:',[s[2] for s in states])
for n in ('S1','S2'):
    print(n)
    ks=[]
    for (na,ua,_),(nb,ub,_) in zip(states,states[1:]):
        dh=POS[nb.split('_')[0]]-POS[na.split('_')[0]]
        if dh==0: continue
        true=dh*1e-3/L[n]*1000; app=reading(ub[n],n)-reading(ua[n],n); k=K0*app/true; ks.append(k)
        print(f'  {na:>9s} -> {nb:<9s} shim {dh:+.1f} mm  true {true:+.3f} mm/m  apparent {app:+.4f}  k = {k:.5f}')
    if ks: print(f'  mean k = {np.mean(ks):.5f}   spread (std) {np.std(ks):.5f} = {100*np.std(ks)/np.mean(ks):.1f} %')
