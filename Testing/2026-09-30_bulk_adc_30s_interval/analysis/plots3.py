import numpy as np, matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from scipy import ndimage
plt.rcParams.update({'figure.dpi':110,'axes.grid':True,'grid.alpha':.3,'font.size':9})
d=np.load('rawspec.npz'); f=d['f']; car=767
names=['S2 (ch0)','B (ch1)','A (ch2)','S1 (ch3)']; col=['tab:green','tab:blue','tab:red','tab:purple']
tri=lambda p,i:p[i-1:i+2].sum()
def dbc(p,c): return 10*np.log10(np.maximum(p,1e-30)/tri(p,car))
fig,ax=plt.subplots(1,3,figsize=(17,4.8))
for c in range(4):
    p=d[f'night_{c}']; ax[0].plot(f,ndimage.uniform_filter1d(dbc(p,c),9),color=col[c],lw=.7,label=names[c])
for h,l in ((car,'1st'),(2*car,'2nd'),(3*car,'3rd')): ax[0].annotate(l,(f[h],-5),ha='center',fontsize=7)
ax[0].set_xlabel('Hz');ax[0].set_ylabel('dB re carrier (per 3.4 Hz bin, night, smoothed)');ax[0].set_title('Raw ADC spectrum, 0-10.4 kHz (no lines below the carrier)');ax[0].legend(fontsize=7);ax[0].set_ylim(-130,5)
b0=car-int(round(100/f[1]));b1=car+int(round(100/f[1]))
for c in (0,3,2):
    for k,ls in (('night','-'),('day','--')):
        p=d[f'{k}_{c}'];ax[1].plot(f[b0:b1+1]-f[car],dbc(p,c)[b0:b1+1],ls,color=col[c],label=f'{names[c]} {k}')
ax[1].set_xlabel('offset from 2604 Hz carrier (Hz)');ax[1].set_ylim(-125,-10);ax[1].set_title('Around the carrier: +/-20 Hz pendulum sidebands (S1/S2 only)');ax[1].legend(fontsize=6,ncol=2)
m=f<300
for c in range(4):
    for k,ls in (('night','-'),('day','--')):
        p=d[f'{k}_{c}'];ax[2].plot(f[m],dbc(p,c)[m],ls,color=col[c],lw=.7)
ax[2].set_xlabel('Hz');ax[2].set_title('0-300 Hz: no mains/other lines; rolloff = AC coupling (~160 Hz corner)');ax[2].set_ylim(-130,-40)
plt.tight_layout();plt.savefig('graphs/14_raw_spectrum.png');plt.close()
# variance bands
bands=['<6','6-15','15-26\n(pendulum)','26-60','60-200','200-1300\n(white)']
rows={'S1 in-phase\nnight':[.3,2.0,59.9,3.6,4.6,29.5],'S1 in-phase\nday':[.3,3.1,89.7,2.2,1.1,3.6],'S2 in-phase\nnight':[.2,2.2,79.3,2.7,1.6,14.0],'S2 in-phase\nday':[.2,2.9,93.8,1.5,.2,1.3],'A amplitude\nnight':[7.4,8.8,4.2,7.8,17.3,54.5],'A amplitude\nday':[7.7,9.4,4.4,8.2,17.6,52.6]}
fig,ax=plt.subplots(figsize=(10,4.5));left=np.zeros(len(rows));cs=plt.cm.viridis(np.linspace(0,.95,6))
for j,b in enumerate(bands):
    v=np.array([r[j] for r in rows.values()]);ax.barh(range(len(rows)),v,left=left,color=cs[j],label=b);left+=v
ax.set_yticks(range(len(rows)));ax.set_yticklabels(rows.keys(),fontsize=8);ax.invert_yaxis();ax.set_xlabel('% of per-cycle variance (frequency of the cycle sequence, Hz)');ax.legend(ncol=6,fontsize=7,loc='lower center',bbox_to_anchor=(.5,1.0));ax.set_title('',y=1.1)
plt.tight_layout();plt.savefig('graphs/15_variance_by_band.png');plt.close()
# trim comparison (day)
lab=['plain 64\n(now)','plain 80','80 trim 8+8\nby amplitude','80 trim 8+8\nby in-phase','plain 96']
jit={'night':[[.128,.368],[.106,.303],[.107,.310],[.107,.309],[.083,.216]],'day':[[.495,1.550],[.428,1.303],[.457,1.395],[.456,1.416],[.309,.932]]}
est={'night':[[.071,.124,.066],[.070,.122,.064],[.070,.138,.087],[.070,.125,.068],[.069,.123,.068]],'day':[[.141,.265,.179],[.141,.262,.176],[.146,.411,.353],[.146,.289,.197],[.141,.267,.181]]}
fig,ax=plt.subplots(1,2,figsize=(14,4.5))
x=np.arange(5);w=.35
for i,(k,c) in enumerate((('night','tab:blue'),('day','tab:orange'))):
    ax[0].bar(x+(i-.5)*w,[v[1] for v in jit[k]],w,color=c,label=f'S2 {k}')
    ax[1].bar(x+(i-.5)*w,[v[2] for v in est[k]],w,color=c,label=f'S1-S2 {k}')
ax[0].set_xticks(x);ax[0].set_xticklabels(lab,fontsize=8);ax[0].set_ylabel('um: within-capture std of batch values (S2)');ax[0].set_title('Live batch jitter');ax[0].legend()
ax[1].set_xticks(x);ax[1].set_xticklabels(lab,fontsize=8);ax[1].set_ylabel('um: 30 s scatter of capture estimate (S1-S2)');ax[1].set_title('Capture-level estimate');ax[1].legend()
plt.tight_layout();plt.savefig('graphs/16_trim80.png');plt.close();print('ok')
