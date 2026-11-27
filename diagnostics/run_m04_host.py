"""Windows x64 MSVC developer prompt: python diagnostics/run_m04_host.py.

Uses installed SDK SwiftShader DLLs as a host library (no emulator/device).
NDK_HEADERS can select sysroot/usr/include; JAVA_HOME selects host JNI ABI.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import tempfile

repo = Path(__file__).resolve().parents[1]
cpp = repo / "app/src/main/cpp"
diag = repo / "diagnostics"
sdk = Path(os.environ.get("ANDROID_HOME", Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk"))
driver = sdk / "emulator/lib64/gles_swiftshader"
jdk = Path(os.environ["JAVA_HOME"]) / "include"
headers = (Path(os.environ["NDK_HEADERS"]) if "NDK_HEADERS" in os.environ else
           next(sdk.glob("ndk/*/toolchains/llvm/prebuilt/*/sysroot/usr/include/EGL/egl.h")).parents[1])

with tempfile.TemporaryDirectory() as directory:
    t = Path(directory)
    for name in ("EGL", "GLES2", "KHR"):
        shutil.copytree(headers / name, t / name)
    shutil.copytree(diag / "test_support/android", t / "android")
    for name in ("libEGL", "libGLESv2"):
        dll = driver / (name + ".dll")
        output = sp.check_output(["dumpbin", "/exports", dll], text=True)
        symbols = re.findall(r"^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+((?:gl|egl)\w+)", output, re.M)
        assert symbols, dll
        (t / (name + ".def")).write_text("LIBRARY " + name + ".dll\nEXPORTS\n" + "\n".join(symbols))
        sp.run(["lib", "/nologo", "/machine:x64", "/def:" + name + ".def"], cwd=t, check=True)
    # Insert only into a disposable copy. Fail if a source anchor moved.
    src = (cpp / "native_bridge.cpp").read_text(encoding="utf-8")
    for marker, hook in [
        ("framePanX = cameraPanX;", "probeLatch(cameraPanX,visibleRevision.load());"),
        ("// No input mutex is held during OpenGL calls.", "probeUnlocked();"),
        ("const double renderCpuMs =", "probeWork(frameDrawCalls,frameUploadBytes);"),
    ]:
        assert src.count(marker) == 1, marker
        src = src.replace(marker, hook + "\n    " + marker)
    (t / "bridge.cpp").write_text("void probeLatch(double,long long); void probeUnlocked();"
                                  "void probeWork(long long,long long);\n" + src, encoding="utf-8")
    sp.run(["cl", "/nologo", "/std:c++20", "/O2", "/EHsc", "/utf-8", "/DNOMINMAX",
            "/DIP_M04_HOST", "/DIP_REPLAY_TEST", "/I.", "/I" + str(cpp), "/I" + str(jdk),
            "/I" + str(jdk / "win32"), diag / "egl_ink_test.cpp", "bridge.cpp",
            *cpp.glob("document/*.cpp"), *cpp.glob("input/*.cpp"), cpp / "stroke/spline.cpp",
            "libEGL.lib", "libGLESv2.lib", "/Fe:probe.exe"], cwd=t, check=True)
    sp.run([t / "probe.exe"], cwd=t, timeout=120, check=True,
           env={**os.environ, "PATH": str(driver) + os.pathsep + os.environ["PATH"]})
