# InfiniPaint — Complete Android + OpenCode Starter

This ZIP is a **real Android Studio prototype** plus an OpenCode engineering framework. It is **not** a completed production drawing/productivity app.

## Contents
- `app/`, Gradle wrapper, `settings.gradle`: corrected Android Java/C++ test application
- `diagnostics/`: native replay and renderer smoke tests
- `AGENTS.md`, `opencode.json`, `.opencode/agents/`, `.opencode/skills/`: AI engineering workflow
- `docs/`, `prompts/`: engineering specifications, quality gates, and audit instructions
- `references/`: original source and documentation archives, plus extracted legacy docs

## Start on Windows
1. Install Git, Android Studio, Android SDK 35, Java 17, CMake 3.22.1, and the required NDK as prompted by Android Studio. The prototype specifies NDK 30.0.16248370; install that if the build requires it.
2. Install OpenCode from https://opencode.ai/docs/ and verify `opencode --version`.
3. Create an empty GitHub repository `Loaide/infinipaint` **without** initializing a README/license/.gitignore. Prefer deleting/recreating the old repo only if its history is genuinely unwanted; otherwise archive it first.
4. Extract this ZIP's **contents** directly into an empty `InfiniPaint` folder (so `settings.gradle` is at the folder root).
5. Open that folder in Android Studio. Allow Gradle sync, install required SDK/NDK components, and build `assembleDebug`.
6. In PowerShell from that folder, `git init -b main`; `git add .`; `git commit -m "Import corrected Android prototype and OpenCode workflow"`.
7. Add the new empty GitHub remote: `git remote add origin https://github.com/Loaide/infinipaint.git`; `git push -u origin main`.
8. Create an audit branch: `git switch -c chore/m00-audit`.
9. Run `opencode`. Use `/connect` to connect an eligible provider and `/models` to choose a supported model. A ChatGPT Pro subscription does **not** automatically include API credits; verify OpenCode's supported ChatGPT sign-in and usage limits before paying.
10. Prompt: `Read AGENTS.md and prompts/M00_FIRST_PROMPT.md. Perform M00 only. Run available checks and stop for approval.`

## Important
- Do not run `git push --force` against the old repository or delete it until you have saved anything valuable.
- `references/*.zip` are intentionally ignored by Git. Local OpenCode can read them; GitHub-based cloud agents cannot unless uploaded separately.
- The Android prototype is not verified here on a physical tablet. Actual shader compilation, latency, and 90Hz pacing require on-device testing.
- The OpenCode config uses conservative permissions; approve commands when asked. Confirm the config against the installed OpenCode version.
- OpenCode needs a live process. Closing your laptop or terminating the process stops local work.
