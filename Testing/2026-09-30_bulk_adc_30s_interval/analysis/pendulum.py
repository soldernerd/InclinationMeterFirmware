import numpy as np, load
from scipy import signal
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
D=A-B; FS=20833.333/8
x1=S1/480/D*2*439*1000; x2=S2/480/D*2*495*1000     # complex, um (nominal)
NF=16384
def spec(z,m):
    z=z[m]-z[m].mean(1,keepdims=True); w=signal.windows.hann(K)
    Z=np.fft.rfft(z*w,NF,axis=1); return np.fft.rfftfreq(NF,1/FS),Z
f,_=spec(x1.real,night)
for nm,m in (('night',night),('day',day)):
    for lab,x in (('S1',x1),('S2',x2)):
        for part,y in (('Re',x.real),('Im',x.imag)):
            fr,Z=spec(y,m); p=(np.abs(Z)**2).mean(0)
            band=(fr>5)&(fr<80); i=np.argmax(p[band]); f0=fr[band][i]
            # peak width (FWHM) of averaged spectrum above local floor
            floor=np.median(p[(fr>200)&(fr<1000)]); pk=p[band][i]
            half=floor+(pk-floor)/2; idx=np.where(band)[0]
            j=idx[i]; lo=j
            while p[lo]>half and lo>0: lo-=1
            hi=j
            while p[hi]>half and hi<len(p)-1: hi+=1
            fw=fr[hi]-fr[lo]
            print('%-5s %s %s: peak %.1f Hz, FWHM %.1f Hz (Q~%.1f), peak/floor %.0f (%.0f dB)'%(nm,lab,part,f0,fw,f0/fw if fw>0 else 0,pk/floor,10*np.log10(pk/floor)))
    # coherence S1-S2 at peak
    fr,Z1=spec(x1.real,m); _,Z2=spec(x2.real,m)
    c=np.abs((Z1*np.conj(Z2)).mean(0))**2/((np.abs(Z1)**2).mean(0)*(np.abs(Z2)**2).mean(0))
    ph=np.degrees(np.angle((Z1*np.conj(Z2)).mean(0)))
    band=(fr>10)&(fr<50);i=np.argmax(c[band]*0+ (np.abs(Z1)**2).mean(0)[band])
    for fq in (10,15,20,25,30,40,50):
        k=np.argmin(abs(fr-fq)); print('   %s coherence S1/S2 Re @%2d Hz: %.2f  phase %.0f deg'%(nm,fq,c[k],ph[k]))
# peak frequency tracking per capture (S1 Re, parabolic on padded spectrum), bin to 1 h
fr,Z=spec(x1.real,np.ones(R,bool)); p=np.abs(Z)**2; band=(fr>12)&(fr<30)
pf=fr[band][np.argmax(p[:,band],1)]
snr=p[:,band].max(1)/np.median(p[:,(fr>200)&(fr<1000)],1)
ok=snr>30
print('per-capture peak freq (S1 Re, 12-30 Hz, only captures with peak/floor>30: %d of %d): median %.1f, p10 %.1f p90 %.1f'%(ok.sum(),R,np.median(pf[ok]),*np.percentile(pf[ok],[10,90])))
for h0 in range(0,24,3):
    m=ok&(th>=h0)&(th<h0+3); print('  %2d-%2dh: n=%3d  median f %.1f  (snr median %.0f)'%(h0,h0+3,m.sum(),np.median(pf[m]) if m.sum() else 0,np.median(snr[(th>=h0)&(th<h0+3)])))
# harmonic: 40 Hz in Re or only in |S|?
for lab,y in (('|x1|-mean',np.abs(x1)),('Re x1',x1.real),('Im x1',x1.imag)):
    fr,Z=spec(y,day); p=(np.abs(Z)**2).mean(0)
    print(' ',lab,'day: p(20Hz)=%.2e p(40Hz)=%.2e ratio 40/20 %.3f'%(p[(fr>17)&(fr<24)].max(),p[(fr>36)&(fr<46)].max(),p[(fr>36)&(fr<46)].max()/p[(fr>17)&(fr<24)].max()))
# save averaged spectra for plot
out={}
for nm,m in (('night',night),('day',day)):
    for lab,x in (('S1',x1),('S2',x2)):
        fr,Z=spec(x.real,m); out[f'{nm}_{lab}']=(np.abs(Z)**2).mean(0)
    fr,Z1=spec(x1.real,m);_,Z2=spec(x2.real,m);out[f'{nm}_diff']=(np.abs(Z1-Z2)**2).mean(0)
np.savez('pend.npz',f=fr,pf=pf,snr=snr,th=th,**out)
