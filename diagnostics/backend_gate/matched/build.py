"""Build/verify an isolated benchmark APK on Windows. Never invokes ADB."""
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess as sp
import sys
import tempfile
import zipfile

src = Path(__file__).resolve().parent
repo = src.parents[2]
sdk = Path(os.environ.get("ANDROID_HOME", Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk"))
bt = sdk / "build-tools/36.0.0"
ndk = sdk / "ndk/30.0.16248370"
bin = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
shader = ndk / "shader-tools/windows-x86_64"
jdk = Path(os.environ["JAVA_HOME"]) / "bin"
jar = sdk / "platforms/android-35/android.jar"
out = Path(sys.argv[1]).resolve()
if out.exists():
    raise FileExistsError(out)
out.parent.mkdir(parents=True, exist_ok=True)
inputs=[src/n for n in ("build.py","runner.cpp","bench.hpp","gl.cpp","vk.cpp","vk.vert","vk.frag","Bench.java","Lifecycle.java","AndroidManifest.xml")]
inputs += [repo/"app/src/main/cpp/native_bridge.cpp",repo/"diagnostics/synthetic_input.csv"]
def hashes():return {f.relative_to(repo).as_posix():hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
source_hashes=hashes()
head=sp.check_output(["git","rev-parse","HEAD"],cwd=repo,text=True).strip()
status=sp.check_output(["git","status","--porcelain","--untracked-files=no"],cwd=repo,text=True).strip()
assert not status,"commit tracked changes before the provenance build"
assert all(sp.run(["git","ls-files","--error-unmatch",f.relative_to(repo).as_posix()],cwd=repo,stdout=sp.DEVNULL).returncode==0 for f in inputs),"untracked build input"
meta=out.with_suffix(".provenance.json")
if meta.exists():raise FileExistsError(meta)
with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
    t = Path(tmp)
    def run(*args):
        sp.run(args, cwd=t, check=True)
    bridge = (repo / "app/src/main/cpp/native_bridge.cpp").read_text()
    text = ""
    for symbol in ["vs","fs"]:
        literals = re.findall(r'const char\* '+symbol+r' = R"\((.*?)\)";', bridge, re.S)
        if len(literals)!=1:
            raise ValueError("v1 shader extraction no longer unique")
        text += f'inline constexpr const char* v1{symbol}=R"({literals[0]})";\n'
    (t/"v1_shaders.hpp").write_text(text)
    # Compile the original ES2 cached draw and coverageLayer, including its
    # scissor, blend state and client-side resolve triangle; no Android app link.
    control=bridge.split("    void bindAttributes() {",1)[1].split("glDisable(GL_SCISSOR_TEST);\n    }",1)
    assert len(control)==2 and "void coverageLayer(" in control[0]
    (t/"v1_draw.inc").write_text("void bindAttributes() {"+control[0]+"glDisable(GL_SCISSOR_TEST);\n}\n")
    with (repo/"diagnostics/synthetic_input.csv").open() as f:
        points = [(float(v["x_px"]),float(v["y_px"])) for v in csv.DictReader(f) if v["outcome"]=="ACCEPTED"][:15]
    assert len(points)==15 and points[0]==(0.,0.)
    (t/"path.inc").write_text("constexpr float path[][2]={"+",".join("{"+f"{x}f,{y}f"+"}" for x,y in points)+"};")
    for stage in ["vert","frag"]:
        run(shader/"glslc.exe", "--target-env=vulkan1.1",src/f"vk.{stage}","-o",stage+".spv")
        run(shader/"spirv-val.exe","--target-env","vulkan1.1",stage+".spv")
        words=struct.iter_unpack("<I",(t/(stage+".spv")).read_bytes())
        (t/f"vk_{stage}.inc").write_text(f"alignas(4) constexpr uint32_t vk_{stage}[]={{"+
                                        ",".join(hex(v[0]) for v in words)+"};\n")
    run(bin/"aarch64-linux-android28-clang++.cmd","-std=c++20","-O2","-Wall","-Wextra",
        "-Werror","-Wno-missing-field-initializers","-shared","-fPIC","-static-libstdc++",
        "-I.",src/"runner.cpp",src/"gl.cpp",src/"vk.cpp","-landroid","-llog","-lEGL",
        "-lGLESv3","-lvulkan","-Wl,--no-undefined","-Wl,-z,max-page-size=16384","-o","libmatched.so")
    symbols=sp.check_output([bin/"llvm-readelf.exe","--dyn-syms","--wide",t/"libmatched.so"],text=True)
    for method in ("start","step","close"):
        assert f"Java_com_infinipaint_diag_matched_Bench_{method}" in symbols
    run(jdk/"javac.exe","--release","8","-cp",jar,"-d",t,src/"Bench.java",src/"Lifecycle.java")
    run(bt/"d8.bat","--lib",jar,"--min-api","28","--output",t,*t.glob("com/**/*.class"))
    run(bt/"aapt2.exe","link","-I",jar,"--manifest",src/"AndroidManifest.xml","-o","base.apk")
    with zipfile.ZipFile(t/"base.apk","a") as z:
        z.write(t/"classes.dex","classes.dex")
        z.write(t/"libmatched.so","lib/arm64-v8a/libmatched.so")
    run(bt/"zipalign.exe","-f","-P","16","4","base.apk","aligned.apk")
    run(jdk/"keytool.exe","-genkeypair","-alias","diag","-keystore","diag.p12","-storepass","diagnostic",
        "-keypass","diagnostic","-dname","CN=Local Matched Diagnostic","-keyalg","RSA","-validity","90")
    run(bt/"apksigner.bat","sign","--ks","diag.p12","--ks-pass","pass:diagnostic","--v4-signing-enabled",
        "false","--out","signed.apk","aligned.apk")
    run(bt/"apksigner.bat","verify","--verbose","signed.apk")
    run(bt/"zipalign.exe","-c","-P","16","4","signed.apk")
    badging=sp.check_output([bt/"aapt2.exe","dump","badging",t/"signed.apk"],text=True)
    assert "package: name='com.infinipaint.diag.matched'" in badging and "uses-permission:" not in badging
    with zipfile.ZipFile(t/"signed.apk") as z:
        assert z.testzip() is None and "classes.dex" in z.namelist()
        assert "lib/arm64-v8a/libmatched.so" in z.namelist()
    signer=sp.check_output([bt/"apksigner.bat","verify","--print-certs",t/"signed.apk"],text=True)
    tools={"ndk":(ndk/"source.properties").read_text(),"build_tools":(bt/"source.properties").read_text(),
           "javac":sp.check_output([jdk/"javac.exe","-version"],stderr=sp.STDOUT,text=True),
           "clang":sp.check_output([bin/"aarch64-linux-android28-clang++.cmd","--version"],text=True),
           "android_jar_sha256":hashlib.sha256(jar.read_bytes()).hexdigest()}
    assert hashes()==source_hashes,"source changed during build"
    assert sp.check_output(["git","rev-parse","HEAD"],cwd=repo,text=True).strip()==head,"HEAD changed during build"
    (t/"signed.apk").rename(out)
    meta.write_text(json.dumps({"git_head":head,"inputs":source_hashes,"tools":tools,"signer":signer,
                               "apk_sha256":hashlib.sha256(out.read_bytes()).hexdigest()},indent=2))
print(out,out.stat().st_size,"sha256="+hashlib.sha256(out.read_bytes()).hexdigest())
