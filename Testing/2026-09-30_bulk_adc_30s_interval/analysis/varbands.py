import numpy as np, load
from scipy import signal
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600; FS=20833.333/8
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]
D=A-B
series={'S1 |S|':np.abs(S1),'S2 |S|':np.abs(S2),'A |A|':np.abs(A),'B |B|':np.abs(B),
        'S1 Re(S/(A-B)) reading':(S1/D).real,'S2 Re(S/(A-B)) reading':(S2/D).real}
bands=[(0,6),(6,15),(15,26),(26,60),(60,200),(200,1302)]
print('fraction of per-cycle variance by band (Hz): ',bands)
for nm,x in series.items():
    for k,m in (('night',night),('day',day)):
        f,p=signal.periodogram(x[m],fs=FS,window='hann',detrend='constant',axis=1,scaling='density')
        pm=p.mean(0); df=f[1]; tot=pm.sum()*df
        fr=[pm[(f>=a)&(f<b)].sum()*df/tot for a,b in bands]
        sd=np.sqrt(tot) if False else np.sqrt((x[m]-x[m].mean(1,keepdims=True)).var(1,ddof=1).mean())
        rel=sd/np.abs(x[m]).mean()*100
        print('%-24s %-5s std %9.4g (%.3f%% of mean) | '%(nm,k,sd,rel)+' '.join('%5.1f%%'%(100*v) for v in fr))
