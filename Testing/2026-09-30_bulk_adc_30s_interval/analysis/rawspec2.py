import numpy as np
from scipy import signal, ndimage
d=np.load('rawspec.npz'); f=d['f']; df=f[1]; car=767
names=['S2(ch0)','B(ch1)','A(ch2)','S1(ch3)']
def tri(p,i): return p[max(i-1,0):i+2].sum()
for c,nm in enumerate(names):
    for k in ('night','day'):
        p=d[f'{k}_{c}']; pc=tri(p,car)
        floor=ndimage.median_filter(p,size=61,mode='nearest')
        # floors in dBc/bin
        ratio=p/floor
        pk,_=signal.find_peaks(ratio,height=8,distance=3)   # >9 dB above local floor
        pk=[i for i in pk if i>1]
        rows=[(f[i],10*np.log10(tri(p,i)/pc),10*np.log10(ratio[i])) for i in pk]
        below=[r for r in rows if r[0]<2604-8]; 
        print('\n==',nm,k,'| floor dBc/bin: <100Hz %.0f  200-1000Hz %.0f  1-2.4k %.0f  3-10k %.0f'%tuple(10*np.log10(np.median(p[(f>a)&(f<b)])/pc) for a,b in ((5,100),(200,1000),(1000,2400),(3000,10000))))
        print('   peaks > 9 dB above local floor, below carrier (Hz, dBc, dB-over-floor):')
        print('   ',', '.join('%.0f(%.0f,+%.0f)'%r for r in sorted(below,key=lambda r:r[1],reverse=True)[:28]))
        above=[r for r in rows if r[0]>2604+8]
        print('   above carrier:',', '.join('%.0f(%.0f,+%.0f)'%r for r in sorted(above,key=lambda r:r[1],reverse=True)[:12]))
