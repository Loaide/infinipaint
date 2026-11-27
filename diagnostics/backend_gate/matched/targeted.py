"""Short, owner-authorized M04 diagnostic. No driver/settings changes; one isolated APK.
Capacity: bounded doubling sweep, confirmation at boundary. Thermal: VKI only.
Not an acceptance test for the production renderer or arbitrary real handwriting.
"""
import argparse
import csv
import gzip
import hashlib
import io
import json
import re
import shutil
import subprocess as sp
import time
from pathlib import Path

import numpy as np
from campaign import summarize

PKG = "com.infinipaint.diag.matched"
FINGERPRINT = b"samsung/gts10fepxx/gts10fep:16/BP2A.250605.031.A3/X626BXXS7BZB7_OJM7BZB7:user/release-keys"
MODES = ("g3i", "vki")
PERIOD = 11_111_111
ROOT = Path(__file__).resolve().parents[3]


def require(ok, msg):
    if not ok:
        raise RuntimeError(msg)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def assess(text, expected, timer):
    s = summarize(text)
    rows = [{k: int(v) for k, v in r.items() if k != "phase"}
            for r in csv.DictReader(io.StringIO(text)) if r["phase"] == "measure"]
    require(len(rows) == expected and all(b["seq"] == a["seq"] + 1 for a, b in zip(rows, rows[1:])),
            "wrong measured frame count or sequence")
    actual = [r["actual_present_ns"] for r in rows]
    require(all(r["upload_bytes"] == 0 for r in rows), "unexpected resident upload")
    sampled = sum(r["seq"] % 8 == 0 for r in rows) if timer == 1 else 0
    s["gpu_expected"]=sampled
    gaps = [b-a for a, b in zip(actual, actual[1:])]
    if not all(x > 0 for x in actual) or not all(g > 0 for g in gaps):
        s.update({"gate_pass":None,"reason":"missing/nonmonotonic presentation feedback","present_span_s":None})
        return s
    missed = [max(0, round(g/PERIOD)-1) for g in gaps]
    rate = (expected-1)*1e9/(actual[-1]-actual[0])
    delivery = len(gaps)/(len(gaps)+sum(missed))
    consecutive = any(m > 1 for m in missed) or any(a and b for a, b in zip(missed, missed[1:]))
    s.update({"observed_hz":rate,"delivery_fraction":delivery,"consecutive_misses":consecutive,
              "present_span_s":(actual[-1]-actual[0])/1e9,
              "gate_pass":rate>=89.5 and delivery>=.995 and s["present_gap_ns"][2]<=11_610_000 and not consecutive})
    # Report worst measured contiguous 1 s window; no independent FrameTimeline join claimed.
    chunks = [rows[i:i+90] for i in range(0,len(rows)-89,90)]
    if chunks:
        worst=max(chunks,key=lambda c:max(r["actual_present_ns"]-r["tick_ns"] for r in c))
        s["worst_age_window"]={"first_seq":worst[0]["seq"],"last_seq":worst[-1]["seq"],
                               "max_tick_to_present_ns":max(r["actual_present_ns"]-r["tick_ns"] for r in worst)}
    return s


class Campaign:
    def __init__(self,args):
        self.a=args
        self.out=args.out
        self.index=0
        self.installed=False
        self.identities={}
        self.prefix=[]

    def adb(self,*cmd,timeout=30):
        return sp.check_output(self.prefix+list(cmd),timeout=timeout)

    def package_present(self):
        return b"package:"+PKG.encode() in self.adb("shell","pm","list","packages","-u",PKG).splitlines()

    def preflight(self):
        meta=json.loads(self.a.apk.with_suffix(".provenance.json").read_text())
        digest=sha(self.a.apk.read_bytes())
        require(digest==self.a.sha256.lower()==meta["apk_sha256"],"APK/provenance hash mismatch")
        require(sp.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip()==meta["git_head"],"build HEAD mismatch")
        for path,d in meta["inputs"].items():
            require(sha((ROOT/path).read_bytes())==d,"source changed: "+path)
        require(shutil.disk_usage(self.out.parent.resolve()).free>=2*1024**3,"need at least 2 GiB free in captures drive")
        devices=sp.check_output(["adb","devices"],text=True).strip().splitlines()
        require(len(devices)==2 and devices[1].split()[1:]==["device"],"need exactly one online tablet")
        self.prefix=["adb","-s",devices[1].split()[0]]
        require(self.adb("shell","getprop","ro.product.model").strip()==b"SM-X626B","wrong device")
        require(self.adb("shell","getprop","ro.build.fingerprint").strip()==FINGERPRINT,"fingerprint changed")
        require(not self.package_present(),"diagnostic package already present; refusing replacement")
        self.out.mkdir(exist_ok=False)
        write_json(self.out/"provenance.json",meta)
        (self.out/"fingerprint.txt").write_bytes(FINGERPRINT+b"\n")
        self.monitor(self.out/"preflight.txt",refresh=True)
        require(b"Success" in self.adb("install",str(self.a.apk),timeout=300),"install failed")
        self.installed=True
        paths=self.adb("shell","pm","path",PKG).decode().splitlines()
        require(len(paths)==1 and paths[0].startswith("package:"),"unexpected installed APK path")
        installed_sha=self.adb("shell","sha256sum",paths[0][8:],timeout=60).decode().split()[0]
        require(installed_sha==digest,"installed APK hash mismatch")
        (self.out/"installed-sha256.txt").write_text(installed_sha+"\n")

    def monitor(self,path,refresh=False,mem=False):
        thermal=self.adb("shell","dumpsys","thermalservice").decode(errors="replace")
        values=re.findall(r"^\s*Thermal Status:\s*(\d+)\s*$",thermal,re.M)
        require(len(values)==1,"thermal status unknown")
        status=int(values[0])
        with path.open("a",encoding="utf-8") as f:
            f.write(f"host_monotonic_s={time.monotonic():.3f} thermal={status}\n")
            if refresh:
                display=self.adb("shell","dumpsys","display").decode(errors="replace")
                rates=re.findall(r"mActiveRenderFrameRate=([\d.]+)",display)
                f.write("active_render_rates="+repr(rates)+"\n")
                require(rates,"active display rate unavailable")
            if mem:
                f.write(self.adb("shell","dumpsys","meminfo",PKG).decode(errors="replace")+"\n")
        require(status<=1,"elevated thermal state; campaign stopped")

    def pull(self,name):
        return self.adb("exec-out","run-as",PKG,"cat","files/"+name,timeout=300)

    def pixels(self,name,dest,identity):
        # Compress before the wireless transfer. Never clear logcat or use shared storage.
        blob=self.adb("exec-out","run-as",PKG,"toybox","gzip","-c","files/"+name,timeout=300)
        data=gzip.decompress(blob)
        w,h=map(int,identity[2:4])
        require(len(data)==w*h*4,"truncated pixel buffer")
        dest.write_bytes(blob)
        return np.frombuffer(data,np.uint8).reshape(h,w,4)

    def receipt(self,filename,prefix,deadline,monitor=None):
        next_sample=time.monotonic()+5
        while time.monotonic()<deadline:
            p=sp.run(self.prefix+["exec-out","run-as",PKG,"cat","files/"+filename],capture_output=True,timeout=15)
            text=p.stdout.decode(errors="replace").strip()
            valid = (text == "ok" or text.startswith("fail ")) if prefix == "" else bool(text and text.startswith(prefix))
            if p.returncode==0 and valid:return text
            if monitor and time.monotonic()>=next_sample:
                monitor();next_sample=time.monotonic()+20
            time.sleep(.5)
        raise RuntimeError("no completion receipt: "+filename)

    def log(self,dest):
        pid=self.adb("shell","pidof",PKG).strip().decode()
        require(pid.isdecimal(),"diagnostic PID missing")
        data=self.adb("logcat","-d","--pid",pid,"-v","threadtime","IP_MATCH:V","*:S",timeout=60)
        dest.write_bytes(data)
        txt=data.decode(errors="replace")
        require(not re.search(r"vk teardown waitIdle=(?!0\b)-?\d+",txt),"Vulkan teardown error")
        require(not re.search(r"vk teardown explicit_buffer_allocated=[1-9]\d*",txt),"explicit Vulkan allocation leak")
        return txt

    def run(self,mode,scene,frames=360,timer=1,thermal=False,landscape=False):
        self.index+=1
        require(not landscape or (scene=="life-pixel" and frames==1 and timer==0),"landscape is lifecycle reference only")
        dest=self.out/f"{self.index:03d}-{mode}-{scene}-n{frames}-t{timer}{'-landscape' if landscape else ''}"
        dest.mkdir()
        print(f"[{self.index}] {mode} {scene} {frames} frames",flush=True)
        self.monitor(dest/"monitor.txt",refresh=True)
        self.adb("shell","am","start","-S","-n",PKG+"/.Bench","--es","api",mode,"--es","scene",scene,
                 "--ei","frames",str(frames),"--ei","timer",str(timer),"--ei","run",str(self.index),
                 *( ("--ez","landscape", "true") if landscape else () ))
        # For a 55k-frame run, sample thermal and refresh in-run; PSS probes are sparse and recorded.
        receipt=self.receipt("status",str(self.index)+" ",time.monotonic()+(1000 if thermal else 90),
                             (lambda:self.monitor(dest/"monitor.txt",refresh=True,mem=True)) if thermal else None)
        (dest/"receipt.txt").write_text(receipt+"\n")
        self.log(dest/"backend.log")
        require(receipt.endswith(" ok"),receipt)
        ident=self.pull("identity.txt").decode().strip().split()
        require(ident[:2]==[mode,scene] and len(ident)==5,"stale/invalid identity")
        (dest/"identity.txt").write_text(" ".join(ident)+"\n")
        size=tuple(map(int,ident[2:4]))
        require(size==((2880,1800) if landscape else (1800,2880)),"unexpected benchmark surface size")
        key=scene.removeprefix("pixel-")+("-landscape" if landscape else "")
        require(self.identities.setdefault(key,ident[2:])==ident[2:],"fixture or size mismatch")
        raw=self.pull("frames.csv").decode()
        (dest/"frames.csv").write_text(raw)
        if scene in ("parity","reverse","life-pixel") or scene.startswith("pixel-"):
            return self.pixels("pixels.rgba",dest/"pixels.rgba.gz",ident)
        stats=assess(raw,frames,timer)
        write_json(dest/"summary.json",stats)
        if stats["gate_pass"] is None:print("    UNKNOWN: "+stats["reason"],flush=True)
        else:print(f"    {'PASS' if stats['gate_pass'] else 'MISS'}  {stats['observed_hz']:.2f} Hz, p99 gap {stats['present_gap_ns'][2]/1e6:.3f} ms",flush=True)
        return stats

    def compare(self,a,b):
        require(a.shape==b.shape,"pixel shapes differ")
        a=a.reshape(-1);b=b.reshape(-1)
        return max(int(np.abs(a[i:i+65536].astype(np.int16)-b[i:i+65536]).max()) for i in range(0,a.size,65536))

    def parity(self,scene):
        reference=self.run("g2",scene,1,0)
        deltas={m:self.compare(reference,self.run(m,scene,1,0)) for m in MODES}
        write_json(self.out/(scene+"-parity.json"),deltas)
        require(max(deltas.values())<=2,"pixel parity failed: "+scene)
        print("    pixel delta "+str(deltas),flush=True)

    def capacity(self):
        bounds={}; passing=[]
        for kind,limit in (("sparse",1024),("overlap",2048)):
            results={m:{} for m in MODES}
            # Start at 128, fall back to 64 only if necessary. Test until each first fails.
            n=128
            while n<=limit:
                for m in MODES if n.bit_length()%2 else reversed(MODES):
                    if any(not s["gate_pass"] for s in results[m].values()):continue
                    results[m][n]=self.run(m,f"{kind}-{n}")
                if all(any(not s["gate_pass"] for s in r.values()) for r in results.values()):break
                n*=2
            for m,r in results.items():
                if not any(s["gate_pass"] for s in r.values()):r[64]=self.run(m,f"{kind}-64")
            # Confirm only candidate boundaries, not the full sweep. 1,000 measured frames.
            for m,r in results.items():
                passed=[n for n,s in r.items() if s["gate_pass"]]
                failed=[n for n,s in r.items() if s["gate_pass"] is False]
                lo=max(passed,default=None);hi=min(failed,default=None)
                if lo is not None:
                    confirmed=self.run(m,f"{kind}-{lo}",1000)
                    if not confirmed["gate_pass"]:lo=None
                    elif m=="vki":passing.append((confirmed["wall_ns"][2],f"{kind}-{lo}"))
                if hi is not None:
                    confirmed=self.run(m,f"{kind}-{hi}",1000)
                    if confirmed["gate_pass"] is not False:hi=None # noisy breakpoint; do not claim a failure boundary
                bounds[f"{kind}-{m}"]={"last_confirmed_pass_strokes":lo,"first_confirmed_fail_strokes":hi,
                                      "tested":{str(n):s["gate_pass"] for n,s in r.items()},
                                      "cap":limit,"chords_per_stroke":16,"unknown_loads":[n for n,s in r.items() if s["gate_pass"] is None]}
            # Check both backends at each distinct confirmed boundary, untimed.
            candidates={b[k] for m in MODES for b in [bounds[f"{kind}-{m}"]]
                        for k in ("last_confirmed_pass_strokes","first_confirmed_fail_strokes") if b[k] is not None}
            for n in sorted(candidates):self.parity(f"pixel-{kind}-{n}")
            write_json(self.out/"capacity.json",bounds)
        require(passing,"no confirmed passing Vulkan load; sustained run not authorized at unknown load")
        # Choose highest p99 frame wall among tested passing VKI loads; proxy, not proof of max capacity.
        return max(passing)[1]

    def lifecycle(self):
        # Fresh same-backend and compatibility references at BOTH physical extents.
        # Neither cross-extent equality nor previous recovered pixels are an oracle.
        refs={}
        for landscape in (True,False):
            refs[landscape]={m:self.run(m,"life-pixel",1,0,landscape=landscape) for m in ("g2","vki")}
        require(self.identities["life-pixel"][-1]==self.identities["life-pixel-landscape"][-1],"fixture differs across extents")
        dest=self.out/"lifecycle";dest.mkdir()
        self.monitor(dest/"monitor.txt",refresh=True,mem=True)
        self.adb("shell","am","start","-S","-n",PKG+"/.Lifecycle")
        print("Lifecycle: suffix, rotation round trip, background then resume",flush=True)
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            status=sp.run(self.prefix+["exec-out","run-as",PKG,"cat","files/life/status"],capture_output=True,timeout=15)
            if status.returncode==0 and status.stdout.startswith(b"fail"):raise RuntimeError(status.stdout.decode())
            ready=sp.run(self.prefix+["exec-out","run-as",PKG,"cat","files/life/background.ready"],capture_output=True,timeout=15)
            if ready.returncode==0 and ready.stdout.strip()==b"ok":break
            time.sleep(.5)
        else:raise RuntimeError("lifecycle background surface destruction not confirmed")
        self.monitor(dest/"monitor.txt",mem=True)
        self.adb("shell","am","start","-n",PKG+"/.Lifecycle","--activity-single-top")
        result=self.receipt("life/status","",time.monotonic()+60)
        (dest/"receipt.txt").write_text(result+"\n")
        logs=self.log(dest/"backend.log")
        require(result=="ok","lifecycle failed: "+result)
        require(logs.count("vk teardown waitIdle=0")>=4,"lifecycle teardown receipts missing")
        # Collect every stage first. Preserve raw data even when an image later fails.
        recovered={}
        for i in range(4):
            sub=dest/f"stage{i}";sub.mkdir()
            require(self.pull(f"life/stage{i}.ok").strip()==b"ok","stage receipt missing")
            ident=self.pull(f"life/stage{i}/identity.txt").decode().strip().split()
            (sub/"identity.txt").write_text(" ".join(ident)+"\n")
            expected_scene="life-live" if i==0 else "life-pixel"
            key="life-pixel-landscape" if i==1 else "life-pixel"
            require(len(ident)==5 and ident[:2]==["vki",expected_scene] and ident[2:]==self.identities[key],"lifecycle fixture or extent changed")
            frames=self.pull(f"life/stage{i}/frames.csv");(sub/"frames.csv").write_bytes(frames)
            rows=list(csv.DictReader(io.StringIO(frames.decode())))
            if i==0:
                require([int(r["seq"]) for r in rows]==list(range(1,9)) and all(int(r["upload_bytes"])==28 for r in rows),"suffix replay incomplete")
            else:
                require(len(rows)==1 and int(rows[0]["upload_bytes"])==0,"unexpected recovery frame")
                recovered[i]=self.pixels(f"life/stage{i}/pixels.rgba",sub/"pixels.rgba.gz",ident)
        deltas={
            "fresh_vki_vs_g2_portrait":self.compare(refs[False]["vki"],refs[False]["g2"]),
            "fresh_vki_vs_g2_landscape":self.compare(refs[True]["vki"],refs[True]["g2"]),
        }
        for i,img in recovered.items():
            deltas[f"stage{i}_vs_fresh_vki"]=self.compare(refs[i==1]["vki"],img)
            deltas[f"stage{i}_vs_g2"]=self.compare(refs[i==1]["g2"],img)
        deltas["portrait_after_background_vs_after_rotation"]=self.compare(recovered[2],recovered[3])
        failed={k:v for k,v in deltas.items() if v>2}
        write_json(dest/"parity.json",{"max_channel_delta":deltas,"threshold":2,"failed":failed,"pass":not failed})
        print("    lifecycle matched-extent pixel deltas "+str(deltas),flush=True)
        require(not failed,"matched-extent parity failed: "+str(failed))

    def execute(self):
        try:
            self.preflight()
            if self.a.lifecycle_only:
                self.lifecycle()
                return
            for s in ("parity","reverse"):self.parity(s)
            load=self.capacity()
            # Run only the leading Vulkan candidate for >=600 s of measured presentation.
            print("Sustained VKI load: "+load,flush=True)
            minimal=self.run("vki",load,1000,-1)
            write_json(self.out/"near-limit-minimal.json",minimal)
            s=self.run("vki",load,55000,1,thermal=True)
            write_json(self.out/"sustained.json",{"load":load,**s})
            require(s["present_span_s"] is not None and s["present_span_s"]>=600,"sustained window shorter than 600 s")
            # Preserve thermal misses as result, don't pretend passing gate.
            self.lifecycle()
            print("Completed targeted campaign. Check capacity.json and sustained.json; results are not owner approval.",flush=True)
        finally:
            if self.installed:
                # Only the package newly installed by this run.
                require(b"Success" in self.adb("uninstall",PKG,timeout=120) and not self.package_present(),"verify diagnostic cleanup")
                (self.out/"cleanup.txt").write_text("isolated package removed\n")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--apk",required=True,type=Path)
    p.add_argument("--sha256",required=True)
    p.add_argument("--out",required=True,type=Path)
    p.add_argument("--authorize-vulkan-only",required=True,action="store_true")
    p.add_argument("--lifecycle-only",action="store_true")
    args=p.parse_args()
    Campaign(args).execute()

if __name__=="__main__":main()
