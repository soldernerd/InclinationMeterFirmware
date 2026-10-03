import numpy as np, load, stats_core as sc
from scipy import signal
t,P,dc=load.get(); P=P[:,1:]; A=np.abs(P); d=np.load('stats.npz'); th=t/3600
T0=21+36.5/60   # wall-clock start, h
wall=(T0+th)%24
q=(th>=(23-T0)%24)&(th<(23-T0)%24+8)      # 23:00-07:00
print('quiet window caps',q.sum(),'t=%.2f-%.2f h'%(th[q].min(),th[q].max()))
o=~q
dph,_=sc.phase_residual_mdeg(P)
def st(x): return np.std(np.diff(x))/np.sqrt(2)
def dev(x): return 1.4826*np.median(np.abs(np.diff(x)-np.median(np.diff(x))))/np.sqrt(2)
FS=20833.333/8
for c,ch in enumerate(sc.CH):
    print('==',ch)
    for nm,m in (('quiet',q),('day/eve',o)):
        sa=np.median(d[f'{ch}_amp_std'][m]); sp=np.median(d[f'{ch}_ph_std'][m])
        ka=np.median(d[f'{ch}_amp_exkurt'][m]);k95=np.percentile(d[f'{ch}_amp_exkurt'][m],95)
        sk=np.median(d[f'{ch}_amp_skew'][m])
        z=np.abs(A[m,:,c]-np.median(A[m,:,c],1,keepdims=True))/sc.mad_sigma(A[m,:,c])[:,None]
        print(f' {nm:7s} amp std med {sa:8.0f} | ph std med {sp:8.1f} mdeg | amp skew {sk:5.2f} exkurt med {ka:5.2f} p95 {k95:5.2f} | P(|z|>4) {np.mean(z>4):.1e} | caps exkurt>2: {np.mean(d[f"{ch}_amp_exkurt"][m]>2)*100:.1f}%')
        a=d[f'{ch}_amp_mean'][m]; 
        print(f'         30s scatter of cap mean: amp {st(a):.1f} cnt ({1e6*st(a)/np.median(a):.1f} ppm)  ph {st(d[f"{ch}_ph_mean"][m]):.2f}')
# ch1/ch2 specifics in quiet; exclude the 7h step region for Allan
r=np.log(d['ch1_amp_mean']/d['ch2_amp_mean'])*1e6
print('ratio ln(a1/a2) 30s scatter ppm: quiet %.2f  other %.2f'%(st(r[q]),st(r[o])))
# PSD in quiet vs other
for c in (0,1,3):
    x=A[:,:,c]-A[:,:,c].mean(1,keepdims=True)
    f,p=signal.welch(x,fs=FS,nperseg=767,window='hann',axis=1,detrend=False)
    for nm,m in (('quiet',q),('other',o)):
        pm=np.sqrt(p[m].mean(0))
        i20=(f>15)&(f<26);ihi=(f>400)
        print(' ch%d %s ASD 15-26Hz peak %.0f  | >400Hz floor %.1f | at 3.4Hz %.0f'%(c,nm,pm[i20].max(),pm[ihi].mean(),pm[1]))
# events within window: biggest capture-to-capture jumps in ch1 phase
p1=np.degrees(np.unwrap(np.radians(((d['ch1_refphase_deg']+22.5)%45)-22.5)))
dp=np.diff(((d['ch1_refphase_deg']+22.5)%45)-22.5)*1000
i=np.argsort(-np.abs(dp))[:3];print('largest ch1 phase jumps (mdeg) at t(h)/wall',[(round(dp[k],1),round(th[k+1],2),'%02d:%02d'%divmod(int(((T0+th[k+1])%24)*60),60)) for k in i])
print('ch1 amp ppm step near 7h:', )
w=(th>6.9)&(th<7.1);print(np.round(1e6*(d['ch1_amp_mean'][w]/np.median(d['ch1_amp_mean'])-1)[::2],0))
