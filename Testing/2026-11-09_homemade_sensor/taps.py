import numpy as np, csv, sys
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
FB=2604.1667/64
r=list(csv.reader(open("data/phasors.csv")))
x=np.array([[float(v) for v in row[:15]] for row in r[1:] if len(row)>=20])
tmin=float(sys.argv[1]) if len(sys.argv)>1 else 2340
x=x[x[:,1]>tmin-5]; t=x[:,1]
iB,qB,iA,qA,iS1,qS1,iS2,qS2=x[:,7:15].T
D=(iA-iB)+1j*(qA-qB); u1=(iS1+1j*qS1)/D; u2=(iS2+1j*qS2)/D
rot=np.exp(-1j*np.angle(u1.mean()))
s1=((u1*rot).real)/429e-6; s2=((u2*rot).real)/429e-6; q1=((u1*rot).imag)/429e-6
k=int(FB); hp=s1-np.convolve(s1,np.ones(k)/k,"same")
print("t range %.0f-%.0f s, %d batches, gaps %d"%(t[0],t[-1],len(t),int((x[:,4]!=0).sum())))
print("S1 range um/m: %.0f .. %.0f (A-B 2.33 V pp: full scale headroom ~ +-%.0f um/m around the offset)"%(s1.min(),s1.max(),1000))
# tap detection: peak |hp| > 30 um/m, min spacing 8 s
idx=[];last=-99
for i in np.argsort(-np.abs(hp)):
    if abs(hp[i])<30: break
    if all(abs(t[i]-t[j])>8 for j in idx): idx.append(i)
idx=sorted(idx); print(len(idx),"taps at t =",[round(t[i],1) for i in idx])
fig,ax=plt.subplots(len(idx) if idx else 1,1,figsize=(10,2.6*max(1,len(idx))),squeeze=False)
for n,i in enumerate(idx):
    a=i-int(0.5*FB); b=min(len(t),i+int(6*FB))
    seg=hp[i:b]; tt=(t[i:b]-t[i])
    # dominant ring frequency from the 0.2-3 s after the peak (Hann, zero padded)
    j0=int(0.15*FB); j1=min(len(seg),int(3.0*FB)); v=seg[j0:j1]*np.hanning(j1-j0)
    F=np.abs(np.fft.rfft(v,4096)); f=np.fft.rfftfreq(4096,1/FB); m=(f>2)&(f<19); fp=f[m][np.argmax(F[m])]
    # decay: envelope = |hilbert-like| via sliding peak per half-cycle, fit log
    env=np.abs(seg); pk=[(tt[q],env[q]) for q in range(1,len(seg)-1) if env[q]>=env[q-1] and env[q]>=env[q+1] and env[q]>0.02*env.max()]
    pk=np.array(pk)
    tau=np.nan
    if len(pk)>4:
        c=np.polyfit(pk[:,0],np.log(pk[:,1]),1); tau=-1/c[0] if c[0]<0 else np.nan
    sat = "CLIPPED?" if np.abs(s1[i])>900 or np.abs(hp[i])>900 else ""
    print("tap %d t=%.1f: peak %.0f um/m, ring f=%.2f Hz, decay tau %.2f s (Q~%.1f) %s"%(n,t[i],hp[i],fp,tau,np.pi*fp*tau if tau==tau else np.nan,sat))
    ax[n,0].plot(t[a:b]-t[i],hp[a:b],label="S1"); ax[n,0].plot(t[a:b]-t[i],s2[a:b]-np.convolve(s2,np.ones(k)/k,"same")[a:b],label="S2",alpha=.6)
    ax[n,0].set_ylabel("um/m"); ax[n,0].grid(alpha=.3)
ax[-1,0].set_xlabel("s after tap"); plt.tight_layout(); plt.savefig("taps.png",dpi=90)
