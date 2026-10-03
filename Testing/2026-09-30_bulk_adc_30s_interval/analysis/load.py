"""Load bulk_adc_log.bin and compute per-cycle phasors (cached to npz)."""
import struct, os, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__))
BIN=os.path.join(HERE,'..','data','bulk_adc_log.bin')
CACHE=os.path.join(HERE,'phasors.npz')
N=8  # samples per cycle (20833.33 Hz / 2604.17 Hz)
def load_raw():
    t=[];raw=[]
    with open(BIN,'rb') as f:
        while True:
            h=f.read(20)
            if len(h)<20:break
            ts,i,c,g=struct.unpack('<dIII',h)
            b=np.frombuffer(f.read(c*12),np.uint8).reshape(c,4,3).astype(np.int32)
            v=b[...,0]|(b[...,1]<<8)|(b[...,2]<<16)
            v=np.where(v&0x800000,v-(1<<24),v)
            t.append(ts);raw.append(v)
    return np.array(t),np.array(raw)   # (R,6144,4)
def phasors(raw):
    R,S,C=raw.shape; K=S//N
    x=raw[:,:K*N,:].reshape(R,K,N,C).astype(np.float64)
    w=np.exp(-2j*np.pi*np.arange(N)/N)
    return (x*w[None,None,:,None]).sum(2)*(2/N)   # (R,K,C) complex, ADC counts peak
def get():
    if os.path.exists(CACHE):
        d=np.load(CACHE);return d['t'],d['P'],d['dc']
    t,raw=load_raw();P=phasors(raw)
    dc=raw[:,:(raw.shape[1]//N)*N,:].reshape(raw.shape[0],-1,N,4).mean(2)
    np.savez_compressed(CACHE,t=t,P=P,dc=dc);return t,P,dc
if __name__=='__main__':
    t,P,dc=get();print(P.shape,dc.shape)
