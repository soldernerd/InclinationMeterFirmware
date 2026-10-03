"""Emulate the firmware reading chain on the raw captures: batch sums -> x'=S/(k(A-B)) -> delta (nominal d0) -> MA8."""
import numpy as np, load
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]
T0=21+36.5/60; quiet=(th>=23-T0)&(th<31-T0)
K_=480.0; D0=(439.0,495.0)
def batches(X,n):
    m=K//n; return X[:,:m*n].reshape(R,m,n).sum(2)
def delta(n):
    d=batches(A,n)-batches(B,n)
    x1=batches(S1,n)/K_/d; x2=batches(S2,n)/K_/d
    return 2*D0[0]*x1.real, 2*D0[1]*x2.real, x1, x2
def st(x): return np.std(np.diff(x))/np.sqrt(2)
if __name__=='__main__':
    d1,d2,x1,x2=delta(64)
    print('batches/capture',d1.shape[1])
    print('mean Re x1 %.3e  Im %.3e ; Re x2 %.3e'%(x1.real.mean(),x1.imag.mean(),x2.real.mean()))
    print('corr sign of capture-mean d1 vs d2 across run: %.3f'%np.corrcoef(d1.mean(1),d2.mean(1))[0,1])
    for nm,m in (('night',quiet),('day  ',~quiet)):
        for k,(dd) in enumerate((d1,d2)):
            w=dd[m]-dd[m].mean(1,keepdims=True)
            # robust within-capture jitter of 24.6ms batch values
            mad=1.4826*np.median(np.abs(w-np.median(w,1,keepdims=True)),1)
            print(nm,'S%d batch(64cyc,24.6ms) within-capture std: median %.4f p90 %.4f p99 %.4f mm ; MAD-sigma median %.4f'%(k+1,np.median(w.std(1,ddof=1)),*np.percentile(w.std(1,ddof=1),[90,99]),np.median(mad)))
    # batch-length scaling of within-capture jitter: n cycles
    print('batch-length scaling (within-capture std of consecutive batch values, mm), night vs day, S1 / S2')
    for n in (8,16,32,64,128,256):
        dd1,dd2,_,_=delta(n); r=[]
        for m in (quiet,~quiet):
            for dd in (dd1,dd2):
                r.append(np.median((dd[m]-dd[m].mean(1,keepdims=True)).std(1,ddof=1)))
        print(' n=%3d (%.1f ms): night S1 %.4f S2 %.4f | day S1 %.4f S2 %.4f   (white-noise ideal from n=8 night: S1 %.4f)'%(n,n*8/20.8333,*r[:2],*r[2:],0))
E=None
