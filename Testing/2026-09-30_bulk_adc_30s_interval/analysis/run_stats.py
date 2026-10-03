import sys,os;sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import numpy as np,load,stats_core as sc,csv
t,P,dc=load.get()
Pfull=P; P=P[:,1:]                      # drop cycle 0 (stale first sample)
A=np.abs(P); dph,ref=sc.phase_residual_mdeg(P)
out={'idx':np.arange(len(t)),'t_s':t}
res={}
for c,n in enumerate(sc.CH):
    da=sc.describe(A[:,:,c]); dp=sc.describe(dph[:,:,c])
    res[(n,'amp')]=da; res[(n,'ph')]=dp
    for k,v in da.items(): out[f'{n}_amp_{k}']=v
    for k,v in dp.items(): out[f'{n}_ph_{k}']=v
    out[f'{n}_refphase_deg']=ref[:,c]
np.savez_compressed('stats.npz',**{k:v for k,v in out.items()})
keys=list(out)
with open('capture_stats.csv','w',newline='') as f:
    w=csv.writer(f);w.writerow(keys)
    for i in range(len(t)): w.writerow([('%.6g'%out[k][i]) for k in keys])
print('wrote capture_stats.csv',len(keys),'cols')
