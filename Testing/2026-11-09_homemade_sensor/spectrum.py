import numpy as np, csv, glob, sys
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
FB=2604.1667/64                      # batch rate 40.69 Hz
r=list(csv.reader(open("data/phasors.csv")))
x=np.array([[float(v) for v in row[:15]] for row in r[1:] if len(row)>=20])
x=x[x[:,6]==0]                       # drop first_batch rows
gap=(x[:,4]!=0)
# longest gapless run
idx=np.flatnonzero(gap); edges=np.r_[0,idx,len(x)]
a,b=max(zip(edges[:-1],edges[1:]),key=lambda e:e[1]-e[0]); x=x[a:b]
print("gapless run: %d batches = %.1f min"%(len(x),len(x)/FB/60))
iB,qB,iA,qA,iS1,qS1,iS2,qS2=x[:,7:15].T
D=(iA-iB)+1j*(qA-qB)
u1=(iS1+1j*qS1)/D; u2=(iS2+1j*qS2)/D
rot=np.exp(-1j*np.angle(u1.mean()))
to_um=lambda z: z*1e6/429.0          # ppm of D -> um/m (1 mV pp per um/m, D = 2.33 V pp)
s1=to_um((u1*rot).real*1e0)*1e-6*1e6/1e6  # keep simple below
s1=(u1*rot).real/429e-6*1e0*1e-0       # ratio -> um/m : 1 um/m = 429 ppm
s1=((u1*rot).real)/429e-6; s2=((u2*rot).real)/429e-6
q1=((u1*rot).imag)/429e-6; Dabs=np.abs(D)/np.abs(D).mean()-1
def welch(v,fs,nseg):
    v=v-v.mean(); L=len(v)//nseg*2-0; L=min(len(v),max(256,len(v)//nseg*2)); step=L//2
    w=np.hanning(L); P=0;n=0
    for s in range(0,len(v)-L+1,step):
        seg=v[s:s+L]; seg=seg-seg.mean(); F=np.fft.rfft(seg*w); P=P+np.abs(F)**2; n+=1
    P=P/n/(fs*np.sum(w**2)); P[1:-1]*=2; return np.fft.rfftfreq(L,1/fs),P
fig,ax=plt.subplots(2,1,figsize=(11,8))
res={}
for name,v in (("S1 in-phase",s1),("S2 in-phase",s2),("S1-S2",s1-s2),("S1 quadrature",q1)):
    f,P=welch(v,FB,8); res[name]=(f,P); ax[0].loglog(f[1:],np.sqrt(P[1:]),label=name)
ax[0].set_xlabel("Hz (batch rate 40.69 Hz, Nyquist 20.3 Hz)"); ax[0].set_ylabel("um/m / sqrt(Hz)"); ax[0].legend(); ax[0].grid(True,which="both",alpha=.3)
ax[0].set_title("Phasor stream spectrum (%.0f min gapless)"%(len(x)/FB/60))
for hz,lab in ((9.31,"50 Hz alias"),(18.62,"100 Hz alias")): ax[0].axvline(hz,color="r",ls=":",lw=.8); ax[0].text(hz,ax[0].get_ylim()[1],lab,fontsize=7,rotation=90,va="top")
f,P=res["S1 in-phase"]; m=(f>0.02)
ax[1].semilogy(f[m],np.sqrt(P[m])); ax[1].set_xlabel("Hz"); ax[1].set_ylabel("S1 um/m / sqrt(Hz)"); ax[1].grid(True,which="both",alpha=.3)
plt.tight_layout(); plt.savefig("spectrum_phasors.png",dpi=110)
# peaks
print("\nS1 in-phase: top spectral peaks (prominent vs local median), f<20 Hz")
for name in ("S1 in-phase","S1-S2"):
    f,P=res[name]; A=np.sqrt(P)
    print(name, "rms total %.3f um/m"%np.std(s1 if name=="S1 in-phase" else s1-s2))
    pk=[]
    for i in range(3,len(f)-3):
        loc=np.median(A[max(1,i-15):i+16])
        if A[i]>A[i-1] and A[i]>A[i+1] and A[i]>4*loc: pk.append((A[i]/loc,f[i],A[i]))
    for p in sorted(pk,reverse=True)[:8]: print("   %6.2f Hz  amp %.4f um/m/rtHz  (%.1fx local floor)"%(p[1],p[2],p[0]))
bands=((0.05,0.5),(0.5,2),(2,3),(3,5),(5,8),(8,11),(11,20))
f,P=res["S1 in-phase"]; df=f[1]-f[0]
print("\nS1 in-phase band rms (um/m):"); 
for lo,hi in bands:
    mk=(f>=lo)&(f<hi); print("  %5.2f-%5.2f Hz: %.4f"%(lo,hi,np.sqrt(np.sum(P[mk])*df)))

print("\n--- S1 in-phase PSD detail (um/m/rtHz), phasor stream ---")
f,P=res["S1 in-phase"]; A=np.sqrt(P)
for hz in (1,2,3,4,5,6,9.31,12,15,18.62):
    i=np.argmin(abs(f-hz)); print("  %5.2f Hz: %.4f"%(f[i],A[i]))
print("  median floor 1-5 Hz: %.4f   5-20 Hz: %.4f"%(np.median(A[(f>1)&(f<5)]),np.median(A[(f>5)&(f<20)])))
# ---- bulk captures: 20.8 kHz, baseband per carrier cycle (2604 Hz sample rate), resolves 50/100/150 Hz directly
CY=2604.1667
k=np.arange(8); W=np.exp(-2j*np.pi*k/8)
acc=[];accD=[]
for fn in sorted(glob.glob("data/bulk_*.npy")):
    d=np.load(fn).astype(float); n=(len(d)//8)*8
    z=(d[:n].reshape(-1,8,4)*W[None,:,None]).sum(1)   # [cycles, ch]  ch0=S2 ch1=B ch2=A ch3=S1
    Dc=z[:,2]-z[:,1]; r1=(z[:,3]/Dc); r1=r1*np.exp(-1j*np.angle(r1.mean()))
    v=r1.real/429e-6; v=v-v.mean()
    w=np.hanning(len(v)); F=np.abs(np.fft.rfft(v*w))**2/(CY*np.sum(w**2)); F[1:-1]*=2
    acc.append(F)
Pb=np.mean(acc,0); fb=np.fft.rfftfreq(len(v),1/CY); Ab=np.sqrt(Pb)
print("\n--- bulk captures (%d x 295 ms, bin %.2f Hz): S1 in-phase um/m/rtHz ---"%(len(acc),fb[1]))
for hz in (10,25,40,50,60,75,100,150,200,300,500,1000):
    i=np.argmin(abs(fb-hz)); lo=max(1,i-6); loc=np.median(Ab[lo:i+7]); print("  %6.1f Hz: %.4f   (%.1fx local median)"%(fb[i],Ab[i],Ab[i]/loc))
pk=[(Ab[i]/np.median(Ab[max(1,i-8):i+9]),fb[i],Ab[i]) for i in range(2,len(fb)-2) if Ab[i]>Ab[i-1] and Ab[i]>Ab[i+1]]
print("strongest narrow peaks above local floor:",[ "%.0f Hz (%.1fx)"%(p[1],p[0]) for p in sorted(pk,reverse=True)[:8]])
fig,a2=plt.subplots(figsize=(11,4)); a2.loglog(fb[1:],Ab[1:]); a2.set_xlabel("Hz"); a2.set_ylabel("S1 um/m/rtHz"); a2.set_title("Bulk captures, baseband S1/D spectrum (avg of %d x 295 ms)"%len(acc))
for h in (50,100,150,200): a2.axvline(h,color="r",ls=":",lw=.7)
a2.grid(True,which="both",alpha=.3); plt.tight_layout(); plt.savefig("spectrum_bulk.png",dpi=110)

f,P=res["S1 in-phase"]; A=np.sqrt(P); m=(f>5)&(f<20)
print("phasor PSD, 5-20 Hz, local maxima:")
for i in np.flatnonzero(m):
    if A[i]>A[i-1] and A[i]>A[i+1] and A[i]>0.05: print("  %.2f Hz  %.3f um/m/rtHz"%(f[i],A[i]))
