import numpy as np

FS = 7042.0          # measured: ~142 us per I2C RSSI read
DUR = 2.0
n = int(FS*DUR); t = np.arange(n)/FS

def run(fm=1000.0, m=0.85, step_db=1.0, rssi_bw=None, fs=FS):
    n=int(fs*DUR); t=np.arange(n)/fs
    env = 1.0 + m*np.sin(2*np.pi*fm*t)           # AM envelope, carrier=1
    env = np.clip(env, 1e-6, None)
    db  = 20*np.log10(env)                        # chip reports log magnitude
    if rssi_bw:                                   # 1-pole LPF inside the chip
        a = np.exp(-2*np.pi*rssi_bw/fs)
        y=np.zeros_like(db); acc=db[0]
        for i,v in enumerate(db):
            acc = a*acc + (1-a)*v; y[i]=acc
        db = y
    q = np.round(db/step_db)*step_db              # 1 dB quantisation
    rec = 10**(q/20.0)                            # back to linear envelope
    rec = rec - rec.mean()
    ref = env - env.mean()
    # match a pure tone at fm; everything else is error
    c = np.exp(-2j*np.pi*fm*t)
    amp = 2*np.abs((rec*c).mean())
    fit = amp*np.sin(2*np.pi*fm*t + np.angle((rec*c).mean())+np.pi/2)
    err = rec - fit
    return 10*np.log10((fit**2).mean()/(err**2).mean())

print("=== A. Quantisation only (RSSI assumed to track perfectly) ===")
print(f"{'tone Hz':>8} {'SNDR dB @1dB step':>20} {'@0.5dB':>10} {'@2dB':>8}")
for fm in (300,700,1000,1600,2500):
    print(f"{fm:>8} {run(fm=fm):>20.1f} {run(fm=fm,step_db=0.5):>10.1f} {run(fm=fm,step_db=2.0):>8.1f}")

print("\n=== B. Effect of modulation depth (1 dB step, 1 kHz tone) ===")
for m in (0.3,0.5,0.85,0.95):
    print(f"  m={m:<5} SNDR = {run(m=m):5.1f} dB")

print("\n=== C. Cost of the chip's RSSI low-pass filter (1 kHz tone, m=0.85) ===")
print(f"{'RSSI LPF -3dB':>14} {'SNDR dB':>9}  {'envelope amplitude kept':>24}")
for bw in (100,200,500,1000,2000,5000,None):
    s=run(rssi_bw=bw)
    keep = 1.0 if bw is None else 1/np.sqrt(1+(1000.0/bw)**2)
    print(f"{str(bw)+' Hz' if bw else 'bypassed':>14} {s:>9.1f}  {20*np.log10(keep):>21.1f} dB")
