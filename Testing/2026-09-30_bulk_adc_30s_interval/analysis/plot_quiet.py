import numpy as np, matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
d=np.load('stats.npz'); th=d['t_s']/3600; T0=21+36.5/60
lo=23-T0; hi=lo+8
def wall(h): return (T0+h)%24
col=['tab:green','tab:blue','tab:red','tab:purple']
def wr(ch):
    r=d[f'{ch}_refphase_deg']; w=((r+22.5)%45)-22.5; return ((w-np.median(w)+22.5)%45)-22.5
fig,ax=plt.subplots(4,1,figsize=(14,11),sharex=True)
for c in (0,3):
    ax[0].semilogy(th,d[f'ch{c}_amp_std'],lw=.5,color=col[c],label=f'ch{c}')
    ax[1].plot(th,d[f'ch{c}_ph_std'],lw=.5,color=col[c],label=f'ch{c}')
ax[0].set_ylabel('amp std (counts)');ax[1].set_ylabel('phase std (mdeg)');ax[1].set_yscale('log')
ax[2].plot(th,1e6*np.log(d['ch1_amp_mean']/d['ch2_amp_mean']),lw=.5,color='k');ax[2].set_ylabel('ln(amp1/amp2) (ppm)')
ax[3].plot(th,wr('ch1')*1000,lw=.6,color=col[1],label='ch1');ax[3].plot(th,wr('ch2')*1000,lw=.6,color=col[2],label='ch2');ax[3].set_ylabel('phase (mdeg)')
for a in ax:
    a.axvspan(lo,hi,color='gold',alpha=.18);a.legend(loc='upper right') if a in (ax[0],ax[1],ax[3]) else None
ticks=np.arange(0,25,3);ax[3].set_xticks(ticks);ax[3].set_xticklabels(['%02d:00'%int(round(wall(x)))if abs(wall(x)-round(wall(x)))>.6 or True else '' for x in ticks]);
ax[3].set_xticks(np.arange(0,25,1)+ (1-T0%1)%1 -1+ (T0%1>0) *0 ,minor=True)
ax[3].set_xlabel('time since start (h)  [tick labels ≈ wall clock, run started 21:36]')
ax[0].set_title('Shaded = 23:00–07:00')
plt.tight_layout();plt.savefig('graphs/9_quiet_window.png');plt.close()
# zoom on 04:30 event
m=(th>6.5)&(th<9.5)
fig,ax=plt.subplots(1,3,figsize=(15,4))
ax[0].plot(th[m],wr('ch1')[m]*1000,color=col[1]);ax[0].plot(th[m],wr('ch2')[m]*1000,color=col[2]);ax[0].set_title('phase (mdeg)')
ax[1].plot(th[m],1e6*(d['ch1_amp_mean'][m]/np.median(d['ch1_amp_mean'])-1),color=col[1]);ax[1].plot(th[m],1e6*(d['ch2_amp_mean'][m]/np.median(d['ch2_amp_mean'])-1),color=col[2]);ax[1].set_title('amp (ppm)')
ax[2].plot(th[m],np.median(d['ch3_amp_mean'])*0+1e6*(d['ch3_amp_mean'][m]/np.median(d['ch3_amp_mean'])-1),color=col[3]);ax[2].set_title('ch3 amp (ppm)')
for a in ax:a.set_xlabel('h since start (7.0 h ≈ 04:36)')
plt.tight_layout();plt.savefig('graphs/10_event_0430_zoom.png');plt.close()
