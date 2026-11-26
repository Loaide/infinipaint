"""One-off host attribution, not an APK benchmark. Run with WSL NDK_HEADERS set.

Build the two immutable trees in a temporary directory; insert clock reads in
the successful lift path only. Emit phases after release. Never build/install
this instrumentation into the app. Phase quantiles are not additive.
"""
import os
from pathlib import Path
import subprocess as sp
import tempfile

BASE = "44f559b0d38c6ec0b194132df01569c7ee9bafec"
APP = "d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f"


def profile(root, sha):
    out = root / sha
    out.mkdir()
    archive = out / "src.tar"
    sp.run(["git", "archive", "-o", str(archive), sha, "diagnostics", "app/src/main/cpp"], check=True)
    sp.run(["tar", "-xf", archive, "-C", out], check=True)
    cpp = out / "app/src/main/cpp"
    bridge = cpp / "native_bridge.cpp"
    text = bridge.read_text()
    head, sep, body = text.partition("Java_com_infinipaint_demo_NativeBridge_end(")
    marks = (["try { finished.emplace_back();", "auto& splineMesh=trace.stableMesh;",
              "auto retired=std::move(it->second);"] if sha == BASE else
             ["auto shape=std::make_shared<FinishedTrace>();", "sceneSink.appended=shape;",
              "auto result=model->commitStroke", "saves.committingStroke(false);",
              "*shape=finishV1", "visibleRevision.store(model->revision());"])
    marks += ["guard.unlock();"]
    count = len(marks) + 1
    body = body.replace("const auto lockStart=profileNs();",
                        f"const auto lockStart=profileNs(); long long t[{count+2}]={{lockStart}};", 1)
    for i, mark in enumerate(marks, 1):
        assert mark in body
        body = body.replace(mark, f"t[{i}]=profileNs(); {mark}", 1)
    after = f"""t[{count}]=profileNs(); retired.reset(); t[{count+1}]=profileNs();
        if(rawSamples==4096) {{
            std::fprintf(stderr,"PHASE");
            for(int j=1;j<={count+1};++j) std::fprintf(stderr," %lld",t[j]-t[j-1]);
            std::fprintf(stderr,"\\n");
        }}
        return JNI_TRUE;"""
    body = body.replace("return JNI_TRUE;", after, 1)
    bridge.write_text("#include <cstdio>\n" + head + sep + body)
    smoke = out / "diagnostics/renderer_smoke.cpp"
    smoke.write_text(smoke.read_text().replace("for(int n:{256,4096})", "for(int n:{4096})"))
    import shutil
    shutil.copyfile(Path(os.environ["NDK_HEADERS"]) / "jni.h", out / "jni.h")
    binary = out / "bench"
    sp.run(["c++", "-std=c++20", "-O2", "-I"+str(out), "-I"+str(cpp),
            "-I"+str(out / "diagnostics/test_support"), smoke, bridge, *cpp.glob("input/*.cpp"),
            cpp / "stroke/spline.cpp", *cpp.glob("document/*.cpp"), "-o", binary], check=True)
    labels = (["lookup", "slot", "finish_publish", "erase"] if sha == BASE else
              ["lookup", "shape_seal", "accounting", "commit_including_observer", "reset_status",
               "finish", "counters_erase"]) + ["unlock_logs", "retire"]
    for mode in (["bench"] if sha == BASE else ["bench", "bench-queue"]):
        run = sp.run([binary, mode], text=True, capture_output=True, check=True)
        rows = [list(map(int, line.split()[1:])) for line in run.stderr.splitlines() if line.startswith("PHASE ")]
        assert len(rows) == 101 and all(len(row) == len(labels) for row in rows)
        print(sha, mode, "n=4096 lifts=101 ns p50/p95/p99/max")
        for label, values in zip(labels, zip(*rows)):
            v = sorted(values)
            print(label, *(v[i] for i in (50, 95, 99, 100)))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        for commit in (BASE, APP):
            profile(Path(temp), commit)
