import numpy as np, matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from scipy import signal
import load
plt.rcParams.update({'figure.dpi':110,'axes.grid':True,'grid.alpha':.3,'font.size':9})
FS=20833.333/8
d=np.load('pend.npz'); f=d['f']
# ---- fig 11: spectra, coherence-ish, pendulum frequency tracking
fig,ax=plt.subplots(1,3,figsize=(16,4.5))
for nm,ls in (('night','-'),('day','--')):
    for lab,c in (('S1','tab:purple'),('S2','tab:green')):
        ax[0].semilogy(f,np.sqrt(d[f'{nm}_{lab}'])/np.sqrt(FS)*1.0,ls,color=c,label=f'{lab} {nm}')
ax[0].set_xlim(0,100);ax[0].axvline(20.35,color='k',ls=':',lw=.8);ax[0].set_xlabel('Hz');ax[0].set_ylabel('relative spectral amplitude (um, Hann, arb. norm.)')
ax[0].set_title('Sensor reading spectrum (dotted: 20.35 Hz)');ax[0].legend(fontsize=7)
ax[1].plot(d['th'],d['pf'],'.',ms=1.5,alpha=.4);ax[1].set_ylim(14,28);ax[1].set_xlabel('time (h)');ax[1].set_ylabel('per-capture peak frequency (Hz)');ax[1].set_title('Peak frequency per capture (S1): 19.9 Hz, scatter 18-21.5')
ax[2].semilogy(d['th'],d['snr'],lw=.5);ax[2].set_xlabel('time (h)');ax[2].set_ylabel('20 Hz peak / HF floor (power)');ax[2].set_title('Pendulum excitation vs time (day/night)')
plt.tight_layout();plt.savefig('graphs/11_pendulum_spectrum.png');plt.close()
# ---- fig 12: filter responses
fr=np.linspace(0.05,60,6000)
def box(L,f_): return np.abs(np.sin(np.pi*f_*L/FS)/(L*np.sin(np.pi*f_/FS)))
def db(x): return 20*np.log10(np.maximum(x,1e-6))
fig,ax=plt.subplots(1,2,figsize=(15,4.8))
curves=[('64-cycle batch only',box(64,fr),'tab:gray'),('current: 8 x 64-cycle batches = 512-cycle boxcar',box(512,fr),'tab:red'),
        ('proposed: triangular (2 cascaded MA8) 960 cycles',box(480,fr)**2,'tab:blue'),('proposed: 3 cascaded boxcars (3 x 384 cyc)',box(384,fr)**3,'tab:green')]
w=signal.windows.hann(1024);Hw=np.abs(np.fft.rfft(w,2**18));fh=np.fft.rfftfreq(2**18,1/FS);Hw/=Hw[0]
for nm,h,c in curves: ax[0].plot(fr,db(h),color=c,label=nm)
ax[0].plot(fh[fh<60],db(Hw[fh<60]),color='tab:orange',label='Hann 1024 cycles (0.39 s)')
ax[0].axvspan(18,21.5,color='gold',alpha=.3,label='pendulum (18-21.5 Hz)');ax[0].axvline(FS/128,color='k',ls=':',lw=.8)
ax[0].set_ylim(-100,3);ax[0].set_xlabel('Hz');ax[0].set_ylabel('gain (dB)');ax[0].legend(fontsize=7);ax[0].set_title('Averaging filter responses (dotted: 20.35 Hz = batch-stream Nyquist)')
# zoom linear low frequency
for nm,h,c in curves: ax[1].plot(fr,db(h),color=c)
ax[1].plot(fh[fh<8],db(Hw[fh<8]),color='tab:orange');ax[1].set_xlim(0,8);ax[1].set_ylim(-12,1);ax[1].set_xlabel('Hz');ax[1].set_title('Passband zoom: what the extra smoothing costs')
plt.tight_layout();plt.savefig('graphs/12_filter_responses.png');plt.close()
# ---- fig 13: strategy comparison bars (values from strategies.py run)
labels=['rect mean 704 cyc\n(now: 11 batches)','flag-filtered mean\n(precision-style)','rect 5 periods\n(654 cyc)','Hann 767','triangular 767','regress DC+12..28Hz']
night=np.array([[0.071,0.124,0.066],[0.191,0.510,0.346],[0.070,0.126,0.071],[0.067,0.101,0.048],[0.066,0.100,0.047],[0.065,0.098,0.045]])
day=np.array([[0.141,0.265,0.179],[0.626,1.879,1.331],[0.142,0.275,0.188],[0.125,0.128,0.075],[0.124,0.129,0.077],[0.124,0.131,0.075]])
fig,ax=plt.subplots(1,2,figsize=(15,4.8))
for a,dat,tt in ((ax[0],night,'Night (quiet)'),(ax[1],day,'Day / evening')):
    x=np.arange(len(labels));w_=.26
    for i,(nm,c) in enumerate((('S1','tab:purple'),('S2','tab:green'),('S1-S2','k'))): a.bar(x+(i-1)*w_,dat[:,i],w_,label=nm,color=c)
    a.set_xticks(x);a.set_xticklabels(labels,fontsize=7);a.set_ylabel('30 s capture-to-capture scatter (um, nominal)');a.set_yscale('log');a.set_title(tt);a.legend()
plt.tight_layout();plt.savefig('graphs/13_strategy_comparison.png');plt.close()
print('ok')
