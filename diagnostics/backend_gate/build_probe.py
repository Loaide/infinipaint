"""Host packaging only. No ADB or Gradle; refuses to overwrite an artifact.
Usage: python diagnostics/backend_gate/build_probe.py captures/backend-cap-v1.apk
"""
import hashlib
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import zipfile

src = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
if out.exists():
    raise FileExistsError(out)
out.parent.mkdir(parents=True, exist_ok=True)
sdk = Path(os.environ.get("ANDROID_HOME", Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk"))
bt = sdk / "build-tools/36.0.0"
cc = sdk / "ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android28-clang++.cmd"
jdk = Path(os.environ["JAVA_HOME"])
with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
    def run(*args):
        sp.run(args, cwd=tmp, check=True)
    run(cc, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
        "-static-libstdc++", src / "probe.cpp", "-landroid", "-llog", "-lEGL", "-lGLESv3",
        "-lvulkan", "-Wl,--no-undefined", "-o", "libprobe.so")
    run(bt / "aapt2.exe", "link", "-I", sdk / "platforms/android-35/android.jar",
        "--manifest", src / "AndroidManifest.xml", "-o", "base.apk")
    with zipfile.ZipFile(Path(tmp) / "base.apk", "a") as z:
        z.write(Path(tmp) / "libprobe.so", "lib/arm64-v8a/libprobe.so")
    run(bt / "zipalign.exe", "-f", "-P", "16", "4", "base.apk", "aligned.apk")
    # Disposable diagnostic signing key; never read app/owner keys. Do not
    # reinstall over an existing package. A later build has a different signer.
    run(jdk / "bin/keytool.exe", "-genkeypair", "-alias", "probe", "-keystore", "probe.p12",
        "-storepass", "diagnostic", "-keypass", "diagnostic", "-dname", "CN=Local Probe",
        "-keyalg", "RSA", "-validity", "90")
    run(bt / "apksigner.bat", "sign", "--ks", "probe.p12", "--ks-pass", "pass:diagnostic",
        "--v4-signing-enabled", "false", "--out", "signed.apk", "aligned.apk")
    run(bt / "apksigner.bat", "verify", "--verbose", "signed.apk")
    (Path(tmp) / "signed.apk").rename(out)
print(out, hashlib.sha256(out.read_bytes()).hexdigest())
