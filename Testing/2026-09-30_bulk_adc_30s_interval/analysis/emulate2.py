import numpy as np, load, stats_core as sc
t,P,dc=load.get(); P=P[:,1:]; R,K,_=P.shape; th=t/3600
S2,B,A,S1=[P[:,:,i] for i in (0,1,2,3)]
T0=21+36.5/60; night=(th>=23-T0)&(th<31-T0); day=~night
D=A-B; D0=(439.,495.)
def st(x): return np.std(np.diff(x))/np.sqrt(2)
y1=2*D0[0]*(S1/480/D).real*1000; y2=2*D0[1]*(S2/480/D).real*1000     # um (nominal firmware units), per cycle
yd=y1-y2; q1=(S1/480/D).imag*2*D0[0]*1000
print('cycle-level reading std (um, within capture): night S1 %.2f S2 %.2f | day S1 %.2f S2 %.2f'%tuple(np.median((y-y.mean(1,keepdims=True)).std(1,ddof=1)[m]) for m in (night,day) for y in (y1,y2)))
# --- within-capture Allan deviation vs tau (cycles)
taus=[1,2,4,8,16,32,64,128,192]
def adev_in(y,m):
    o=[]
    for n in taus:
        k=K//n; z=y[m][:,:k*n].reshape(m.sum(),k,n).mean(2); o.append(np.sqrt(0.5*np.mean(np.diff(z,axis=1)**2)))
    return np.array(o)
out={}
for nm,m in (('night',night),('day',day)):
    for lab,y in (('S1',y1),('S2',y2),('S1-S2',yd)):
        out[(nm,lab)]=adev_in(y,m)
print('within-capture Allan dev (um) vs tau cycles',taus,'(ms:',[round(n*8/20.8333,1) for n in taus],')')
for k,v in out.items(): print(' ',k,np.round(v,3))
# --- capture-level estimators of the reading: 30 s scatter
def est(y):
    d={}
    d['mean']=y.mean(1); d['median']=np.median(y,1); d['trim10']=sc.stats.trim_mean(y,.1,axis=1); d['trim25']=sc.stats.trim_mean(y,.25,axis=1); d['huber']=sc.huber_loc(y)
    # batch-level (firmware-like): 64-cycle coherent batches, mean of batches
    return d
print('30-s scatter of capture-level reading (um): estimator | night S1 S2 S1-S2 | day S1 S2 S1-S2')
res={}
for e in ['mean','median','trim10','trim25','huber']:
    row=[]
    for m in (night,day):
        for y in (y1,y2,yd):
            row.append(st(est(y)[e][m]))
    res[e]=row; print('  %-7s'%e,' '.join('%7.3f'%v for v in row[:3]),'|',' '.join('%7.3f'%v for v in row[3:]))
# firmware-like: 64-cycle batch sums -> divide -> mean of batches, flag-filtered mean
def batch_readings(n=64):
    m=K//n
    sums=lambda X:X[:,:m*n].reshape(R,m,n).sum(2)
    Db=sums(A)-sums(B); x1=sums(S1)/480/Db; x2=sums(S2)/480/Db
    return 2*D0[0]*x1*1000, 2*D0[1]*x2*1000    # complex um
b1,b2=batch_readings()
def fw_flag(res_im):
    # emulate: step of residual vs EWMA(32) of good steps, reject >3x ; reset each capture
    Rn,M=res_im.shape; good=np.ones((Rn,M),bool)
    for r in range(Rn):
        prev=res_im[r,0]; base=None
        for k in range(1,M):
            s=abs(res_im[r,k]-prev); prev=res_im[r,k]
            if base is None: base=s; continue
            g=s<=3*base; good[r,k]=g
            if g: base+=(s-base)/32
    return good
g1=fw_flag(b1.imag); g2=fw_flag(b2.imag)
print('firmware-style quality flag rejects: night S1 %.1f%% S2 %.1f%% | day S1 %.1f%% S2 %.1f%%'%(100*(1-g1[night].mean()),100*(1-g2[night].mean()),100*(1-g1[day].mean()),100*(1-g2[day].mean())))
def gm(b,g):
    r=b.real.copy(); w=g.astype(float); w[:,0]=1
    return (r*w).sum(1)/w.sum(1)
for nm,fn in (('11-batch mean (precision-like)',lambda b,g:b.real.mean(1)),('flag-filtered mean',gm),('batch median',lambda b,g:np.median(b.real,1))):
    row=[]
    for m in (night,day):
        for b,g in ((b1,g1),(b2,g2)):
            row.append(st(fn(b,g)[m]))
        row.append(st((fn(b1,g1)-fn(b2,g2))[m]))
    print('  %-32s'%nm,' '.join('%7.3f'%v for v in row[:3]),'|',' '.join('%7.3f'%v for v in row[3:]))
# ratiometric benefit on capture level (mean over capture)
mD=np.median(np.abs(D.mean(1)))
ph=lambda X: X.mean(1)
Dm=D.mean(1)
r_full1=2*D0[0]*1000*(S1.mean(1)/480/Dm).real
r_mag1=2*D0[0]*1000*(S1.mean(1)*np.exp(-1j*np.angle(Dm))/480/mD).real       # phase ref only, no amplitude division
r_none1=2*D0[0]*1000*(np.abs(S1.mean(1))/480/mD)*np.sign(1)                  # |S| alone, fixed normalisation
for nm,r in (('full ratio S/(A-B)',r_full1),('phase-ref only (no amp division)',r_mag1),('|S| alone',r_none1)):
    print('  S1 30s scatter %-34s night %.3f um  day %.3f um | 1h-ish Allan(120 caps) %.3f'%(nm,st(r[night]),st(r[day]),0))
def allan_c(x,m):
    n=len(x)//m; y=x[:n*m].reshape(n,m).mean(1); return np.sqrt(.5*np.mean(np.diff(y)**2))
ms=[1,2,4,10,20,60,120,240,480]
print('capture-level Allan (tau=30s*m) m=',ms)
for nm,r in (('S1 full ratio',r_full1),('S1 phase-ref only',r_mag1),('S1 |S| alone',r_none1),('S2 full ratio',2*D0[1]*1000*(S2.mean(1)/480/Dm).real),('S1-S2 diff',r_full1-2*D0[1]*1000*(S2.mean(1)/480/Dm).real)):
    print('  %-20s'%nm,np.round([allan_c(r,m) for m in ms],3))
np.savez('emu.npz',taus=taus,**{f'{k[0]}_{k[1]}':v for k,v in out.items()},night=night)
