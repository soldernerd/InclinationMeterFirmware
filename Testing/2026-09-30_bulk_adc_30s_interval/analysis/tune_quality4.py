import numpy as np, io, contextlib
with contextlib.redirect_stdout(io.StringIO()):
    from tune_quality import *
    from tune_quality2 import min_floor
q1,q2=q_of(r1),q_of(r2)
W=81; eps=np.log(2)/(20*60*RB)
m81_1=rolling_mean(q1,W); m81_2=rolling_mean(q2,W)
f1=min_floor(m81_1,eps); f2=min_floor(m81_2,eps)
m25_1=rolling_mean(q1,25); m25_2=rolling_mean(q2,25)
R1=m25_1/f1; R2=m25_2/f2; Rm=np.fmax(R1,R2)
hw=signal.windows.hann(27)[1:-1]; hw/=hw.sum()
H=np.full(n,np.nan); H[24:]=np.convolve(y1-y2,hw[::-1],mode='valid')
idx=np.arange(W,n,10)               # output instants (every 10th batch, window full, floor defined)
Hd=H[idx]; Rd=Rm[idx]; nt=night[idx]
step=np.abs(np.diff(Hd)); 
print('display outputs: %d. |step to next output| median: night %.4f day %.4f'%(len(idx),np.median(step[nt[:-1]]),np.median(step[~nt[:-1]])))
for K in (4,5,6,8,10,12,16):
    fl=Rd>K
    print('K_disp=%-3g flagged: night %4.1f%%  day %4.1f%% | mean|step to next| flagged %.4f vs unflagged %.4f (day only: %.4f vs %.4f)'%(K,100*fl[nt].mean(),100*fl[~nt].mean(),step[fl[:-1]].mean(),step[~fl[:-1]].mean(),step[(fl[:-1])&(~nt[:-1])].mean(),step[(~fl[:-1])&(~nt[:-1])].mean()))
