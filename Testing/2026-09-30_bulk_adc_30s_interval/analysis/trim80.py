import numpy as np, load
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]; D=A-B
def st(x): return np.std(np.diff(x))/np.sqrt(2)
D0=(439.,495.)
def batch_values(S,d0,n,keep=None,key='amp'):
    nb=K//n
    Sb=S[:,:nb*n].reshape(R,nb,n); Db=D[:,:nb*n].reshape(R,nb,n)
    Dbar=Db.mean(2)
    if keep is None or keep==n:
        Sbar=Sb.mean(2)
    else:
        trim=(n-keep)//2
        if key=='amp': k=np.abs(Sb)
        elif key=='re':
            u=np.conj(Db)/np.abs(Db); k=(Sb*u).real
        elif key=='im':
            u=np.conj(Db)/np.abs(Db); k=(Sb*u).imag
        order=np.argsort(k,axis=2); rank=np.argsort(order,axis=2)
        m=(rank>=trim)&(rank<n-trim)
        Sbar=(Sb*m).sum(2)/keep
    return 2*d0*1000*(Sbar/480/Dbar).real      # um nominal (R,nb)
def report(nm,fn):
    r=[]
    for m in (night,day):
        v1=fn(S1,D0[0]);v2=fn(S2,D0[1])
        # (a) batch-level jitter: within-capture std of consecutive batch values
        ja=[np.median(np.std(v-v.mean(1,keepdims=True),1,ddof=1)[m]) for v in (v1,v2)]
        # (b) capture-level estimate (mean of batches) 30 s scatter
        e1=v1.mean(1);e2=v2.mean(1)
        r.append((ja[0],ja[1],st(e1[m]),st(e2[m]),st((e1-e2)[m])))
    print('  %-34s night: jit S1 %.3f S2 %.3f | est S1 %.3f S2 %.3f S1-S2 %.3f || day: jit S1 %.3f S2 %.3f | est S1 %.3f S2 %.3f S1-S2 %.3f'%((nm,)+r[0]+r[1]))
print('(um nominal units; jit = within-capture std of batch values = what a live reader sees before MA; est = 30 s scatter of mean of all batches in the capture)')
report('plain 64 (now), 704 cyc',lambda S,d:batch_values(S,d,64))
report('plain 80, 720 cyc',lambda S,d:batch_values(S,d,80))
for key in ('amp','re','im'):
    report('80 -> trim 8+8 by %s -> 64'%key,lambda S,d,key=key:batch_values(S,d,80,64,key))
report('plain 96, 768 cyc',lambda S,d:batch_values(S,d,96))
report('64 -> trim 4+4 by re -> 56',lambda S,d:batch_values(S,d,64,56,'re'))
report('plain 56 (same count as trimmed)',lambda S,d:batch_values(S,d,56))
# trimming bias check: signed difference trimmed vs plain at capture level
v_pl=batch_values(S1,D0[0],80).mean(1); v_tr=batch_values(S1,D0[0],80,64,'amp').mean(1)
print('S1 80-trim(amp) minus plain-80 per capture: mean %.4f um, std %.4f um (day %.4f, night %.4f)'%((v_tr-v_pl).mean(),(v_tr-v_pl).std(),(v_tr-v_pl)[day].std(),(v_tr-v_pl)[night].std()))
