"""Per-capture statistics (classical + robust) on per-cycle phasors."""
import numpy as np
from scipy import stats
CH=['ch0','ch1','ch2','ch3']

def mad_sigma(x,axis=1):
    m=np.median(x,axis,keepdims=True)
    return 1.4826*np.median(np.abs(x-m),axis)

def clip_mask(x,k=3.5,it=3):
    """Iterative MAD sigma-clip along axis 1. True = kept."""
    keep=np.ones(x.shape,bool)
    for _ in range(it):
        xm=np.where(keep,x,np.nan)
        m=np.nanmedian(xm,1,keepdims=True)
        s=1.4826*np.nanmedian(np.abs(xm-m),1,keepdims=True)
        keep=np.abs(x-m)<=k*s
    return keep

def huber_loc(x,c=1.345,it=8):
    m=np.median(x,1,keepdims=True); s=mad_sigma(x)[:,None]
    for _ in range(it):
        r=(x-m)/s; w=np.minimum(1,c/np.maximum(np.abs(r),1e-12))
        m=(w*x).sum(1,keepdims=True)/w.sum(1,keepdims=True)
    return m[:,0]

def moors_kurt(x):
    q=np.percentile(x,[12.5,25,37.5,62.5,75,87.5],axis=1)
    return ((q[5]-q[1])+(q[4]-q[0]))/(q[3]-q[2])-1.2330   # excess-like (Normal -> 0)
def bowley_skew(x):
    q1,q2,q3=np.percentile(x,[25,50,75],axis=1);return (q3+q1-2*q2)/(q3-q1)

def describe(x):
    """x: (R,K). returns dict of arrays (R,)."""
    d={}
    d['mean']=x.mean(1); d['median']=np.median(x,1); d['std']=x.std(1,ddof=1)
    d['skew']=stats.skew(x,axis=1); d['exkurt']=stats.kurtosis(x,axis=1)
    # robust
    d['trim10']=stats.trim_mean(x,0.10,axis=1)
    d['trim25']=stats.trim_mean(x,0.25,axis=1)
    d['huber']=huber_loc(x)
    d['mad_sigma']=mad_sigma(x)
    d['iqr_sigma']=(np.percentile(x,75,1)-np.percentile(x,25,1))/1.349
    keep=clip_mask(x)
    n=keep.sum(1); xm=np.where(keep,x,np.nan)
    d['clip_mean']=np.nanmean(xm,1); d['clip_std']=np.nanstd(xm,1,ddof=1)
    d['clip_n']=x.shape[1]-n
    d['bowley_skew']=bowley_skew(x); d['moors_kurt']=moors_kurt(x)
    return d

def phase_residual_mdeg(P):
    """P (R,K,C) complex -> per-cycle phase deviation from capture median phasor, mdeg, and capture reference angle (deg)."""
    ref=np.median(P.real,1,keepdims=True)+1j*np.median(P.imag,1,keepdims=True)
    dph=np.degrees(np.angle(P*np.conj(ref)))*1000.0
    return dph,np.degrees(np.angle(ref[:,0,:]))
