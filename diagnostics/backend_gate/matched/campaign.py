"""DEVICE ACTIONS. Run only with separate written approval. --help is host-only.
Uses one new package; never changes settings, forces a driver or clears logcat.
"""
import argparse
import csv
import gzip
import hashlib
import io
import json
import math
from pathlib import Path
import subprocess as sp
import time

import numpy as np

pkg="com.infinipaint.diag.matched"
modes=["g2","g3e","g3i","vke","vki"]
def require(ok,reason):
    if not ok: raise RuntimeError(reason)

def summarize(text):
    rows=[{k:int(v) for k,v in r.items() if k!="phase"} for r in csv.DictReader(io.StringIO(text)) if r["phase"]=="measure"]
    if not rows:return {}
    def dist(values):
        values=sorted(values)
        return ([values[math.ceil(len(values)*p)-1] for p in (.5,.95,.99,1)] if values else None)
    actual=[r for r in rows if r["actual_present_ns"]>0]
    # Never infer a miss across frames whose feedback is absent.
    gaps=[b["actual_present_ns"]-a["actual_present_ns"] for a,b in zip(actual,actual[1:])
          if b["seq"]==a["seq"]+1 and b["actual_present_ns"]>a["actual_present_ns"]]
    return {"n":len(rows),"gpu_valid":sum(r["gpu_ns"]>0 for r in rows),"present_valid":len(actual),
            "wall_ns":dist(r["end_ns"]-r["begin_ns"] for r in rows),
            "thread_cpu_ns":dist(r["thread_cpu_ns"] for r in rows if r["thread_cpu_ns"]>0),
            "gpu_ns":dist(r["gpu_ns"] for r in rows if r["gpu_ns"]>0),
            "present_gap_ns":dist(gaps),"comparable_intervals":len(gaps),
            "tick_to_present_ns":dist(r["actual_present_ns"]-r["tick_ns"] for r in actual),
            "refreshes_missed_if_90Hz":sum(max(0,round(g/11111111)-1) for g in gaps),
            **{k:dist(r[k] for r in rows) for k in ("sync_ns","submit_ns","present_call_ns","draws","upload_bytes")}}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--authorized-phase",choices=["smoke","selection"],required=True)
    p.add_argument("--apk",type=Path,required=True)
    p.add_argument("--sha256",required=True)
    p.add_argument("--out",type=Path,required=True)
    args=p.parse_args()
    require(hashlib.sha256(args.apk.read_bytes()).hexdigest()==args.sha256.lower(),"APK hash mismatch")
    devices=sp.check_output(["adb","devices"],text=True).strip().splitlines()
    require(len(devices)==2 and devices[1].split()[1:]==["device"],"need exactly one online device")
    prefix=["adb","-s",devices[1].split()[0]]
    def adb(*cmd):
        return sp.check_output(prefix+list(cmd),timeout=90)
    require(adb("shell","getprop","ro.product.model").strip()==b"SM-X626B","wrong model")
    fingerprint=adb("shell","getprop","ro.build.fingerprint").strip()
    require(fingerprint==b"samsung/gts10fepxx/gts10fep:16/BP2A.250605.031.A3/X626BXXS7BZB7_OJM7BZB7:user/release-keys","build mismatch; stop")
    def installed():
        return b"package:"+pkg.encode() in adb("shell","pm","list","packages","-u",pkg).splitlines()
    require(not installed(),"existing diagnostic package; refusing replacement")
    args.out.mkdir(parents=True,exist_ok=False)
    (args.out/"fingerprint.txt").write_bytes(fingerprint+b"\n")
    confirmed=False
    try:
        require(b"Success" in adb("install",str(args.apk)), "new install failed")
        confirmed=True
        reference={};identities={};index=0
        def run(mode,scene,frames,timer):
            nonlocal index
            index+=1
            dest=args.out/f"{index:03d}-{mode}-{scene}-t{timer}"
            dest.mkdir()
            thermal=adb("shell","dumpsys","thermalservice").decode(errors="replace").splitlines()
            status=[s.strip() for s in thermal if s.strip().startswith("Thermal Status:")]
            require(len(status)==1 and int(status[0].split(":")[1])<=1,"unknown/elevated thermal state; stop")
            (dest/"thermal.txt").write_text(status[0]+"\n")
            adb("shell","am","start","-S","-n",pkg+"/.Bench","--es","api",mode,"--es","scene",scene,
                "--ei","frames",str(frames),"--ei","timer",str(timer),"--ei","run",str(index))
            def pull(file):
                return adb("exec-out","run-as",pkg,"cat","files/"+file)
            deadline=time.monotonic()+max(120,frames/20+60)
            while time.monotonic()<deadline:
                res=sp.run(prefix+["exec-out","run-as",pkg,"cat","files/status"],capture_output=True,timeout=15)
                fields=res.stdout.decode(errors="replace").strip().split(" ",1)
                if len(fields)==2 and fields[0]==str(index):break
                time.sleep(.5)
            else:raise RuntimeError(f"{dest}: no new completion receipt")
            (dest/"receipt.txt").write_text(res.stdout.decode())
            # am start -S yielded a fresh process. Restrict diagnostics to its PID
            # and two diagnostic tags; never clear/read the general log buffer.
            pid=adb("shell","pidof",pkg).strip().decode()
            require(pid.isdecimal(),"diagnostic process no longer available for log capture")
            (dest/"backend.log").write_bytes(adb("logcat","-d","--pid",pid,"IP_MATCH:I","*:S"))
            require(fields[1]=="ok",fields[1])
            identity=pull("identity.txt").decode().strip().split()
            require(identity[:2]==[mode,scene],"stale identity")
            k=(scene,frames);match=identity[2:]
            require(identities.setdefault(k,match)==match,"surface size or fixture hash changed")
            (dest/"identity.txt").write_text(" ".join(identity)+"\n")
            text=pull("frames.csv").decode()
            (dest/"frames.csv").write_text(text)
            if scene in ("parity","reverse"):
                data=pull("pixels.rgba")
                require(len(data)==int(identity[2])*int(identity[3])*4,"truncated pixel buffer")
                with gzip.open(dest/"pixels.rgba.gz","wb") as f:f.write(data)
                got=np.frombuffer(data,np.uint8)
                if mode=="g2":reference[scene]=got
                ref=reference[scene]
                # Bounded conversion scratch, max over every RGBA channel.
                delta=max(int(np.abs(got[i:i+65536].astype(np.int16)-ref[i:i+65536]).max())
                          for i in range(0,len(got),65536))
                (dest/"parity.txt").write_text(f"max_rgba_delta_vs_g2={delta}\n")
                require(delta<=2,"pixel parity failed; no timing comparison")
            else:
                stats=summarize(text);require(stats["n"]==frames,"wrong measured window")
                (dest/"summary.json").write_text(json.dumps(stats,indent=2))
                require(stats["present_valid"]>0 and (timer!=1 or stats["gpu_valid"]>0),
                        "missing functional timing; stop for attribution diagnosis")
        for scene in ["parity","reverse"]:
            for m in modes:run(m,scene,1,0)
        if args.authorized_phase=="smoke":
            for m in modes:run(m,"live",32,1)
        else:
            for rep in range(3):
                for scene in ["pan","live","dense2","dense16","dense64"]:
                    for m in modes[rep:]+modes[:rep]:
                        for t in ([-1,0,1][rep:]+[-1,0,1][:rep] if scene=="pan" else [1]):
                            run(m,scene,1000,t)
    finally:
        if confirmed:
            require(b"Success" in adb("uninstall",pkg) and not installed(),"verify diagnostic cleanup")
if __name__=="__main__":main()
