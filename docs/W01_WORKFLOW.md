# W01 — engineering workflow

## Activate in OpenCode 2.4.1

From the repository, use `opencode reload` (or `/reload` in the TUI), `opencode debug config` and `opencode debug agents`. If `opencode` is not on PATH, invoke the installed executable by its full path. Reload worked without restarting the service. In a fresh InfiniPaint session verify the primary agent is `infinipaint-lead` **and** use the model picker to select **GPT-6.2 Astra, Standard, Medium** if the session shows a different selection. A primary session stores its model separately; changing agents or reloading does not force an existing session to switch. Use the picker for deliberate escalation and revert afterward. No provider, credential or global configuration changed.

|Agent|Configured provider reference|Access|
|---|---|---|
|`infinipaint-lead`|`openai/gpt-6.2-astra#medium`|Project edits allowed; shell asks except narrow Git queries; external paths, web fetch and child launch ask. Owner authorization still required for protected actions.|
|`graphics-reviewer`|`openai/gpt-6.2-astra#high`|Repository read/search and skill access only.|
|`performance-reviewer`|`openai/gpt-6.2-sol#high`|Same read-only policy.|
|`qa-engineer`|`openai/gpt-6.2-sol#medium`|Read-only except `ask` for edits under `app/src/test/` and `app/src/androidTest/`; only explicitly assigned files.|
|`general`|`openai/gpt-6.2-sol#medium`|Read/search and skill access; edits ask and need explicit file ownership. Lead runs commands.|
|`explore`|`openai/gpt-6-luna` (default effort)|Local read/search only.|
|`compaction`|`openai/gpt-6.2-sol#medium`|Hidden summary routing; native provider compaction, if selected elsewhere, uses the session model.|

The five visible child agents deny shell, recursive delegation, external-directory access and Code Mode. Permission composition is enforced by OpenCode, not an OS sandbox. QA's test-directory rules are a ceiling; the delegation packet identifies exact files. Use one-time approvals, not “always” or `--auto`, when relying on edit approval. Concurrency is a lead policy in [AGENTS.md](../AGENTS.md), not a server quota. No arbitrary step caps, cache warming, cache settings or context-budget changes were added.

## Windows verification and usage

```powershell
powershell -NoProfile -File scripts/engineering.ps1 -Mode State
powershell -NoProfile -File scripts/engineering.ps1 -Mode Artifacts
powershell -NoProfile -File scripts/engineering.ps1 -Mode Focused
powershell -NoProfile -File scripts/engineering.ps1 -Mode Comprehensive
powershell -NoProfile -File scripts/engineering.ps1 -Mode Usage -OpenCode opencode
```

`Artifacts` reads existing JUnit XML without running tests (`-ResultsDir` selects a different XML directory). `Focused` runs the debug JVM test task; `Comprehensive` adds the APK, configured ABIs, lint and tracked diff check. Use the M01 [toolchain](../reports/LATEST.md). A cached or up-to-date task is printed as such; `NO-SOURCE`, missing/all-skipped results and failures are rejected. See [host diagnostics](../diagnostics/README.md) for the separate replay and mock GLES commands and [tablet capture](M01_FRAME_CAPTURE.md) for device procedures. Inspect and stage intended untracked changes before the final `git diff --cached --check`.

Gradle and usage outputs are retained under ignored `captures/workflow/`; the CLI returns concise counts, exit codes and paths. `Usage` queries project-local OpenCode statistics, not remaining plan allowance; it does not prompt a model. Helpers never install, clear, merge or push. On-demand skills: `engineering-verification`, `tablet-acceptance`, `performance-lab`, `milestone-closure`.

## Supplied usage baseline

All 274 model steps were `openai/gpt-6.2-astra-fast#default`: approximately 8.2M total reported tokens, 670,300 uncached input, 70,800 output, 2,600 reasoning, approximately 7.5M cache read and zero cache write; **91.8%** cached input and **$0.00** direct reported cost. These are owner-supplied rounded figures, not a measure of remaining subscription quota. A later lifetime stats snapshot includes W01 activity and is not a like-for-like comparison. Compare future verification checkpoints using similar task scope and separately track steps, uncached input, output and cache reads. No W01 savings claim yet.

## Validation and limits

Started at verified local and live `main` `93e2344b9b078d04da30727faa8817d3e85a3e2c`. Inspected project config, four Markdown agents, skill definitions, engineering rules, handoff, source paths and diagnostic recipes. The live provider catalog verified model IDs and variants. OpenCode 2.4.1 parsed the native config and discovered all seven configurations; `debug agents` ordered rules were checked for lead approval, reviewer shell/edit/delegation denial, QA test-path approval and production denial, and general edit approval. New skills were discovered. This validates loaded policy, not an adversarial tool invocation or model dispatch. Confirm the primary model in a fresh session, and observe routing/compaction during real work rather than generating synthetic requests.

PowerShell parser checks, live `State` / `Usage`, and `Artifacts` against the existing 10 M01 test cases passed. An isolated fake-wrapper fixture verified success, cached results, exit 7 propagation and rejection of `NO-SOURCE` with stale XML; missing XML, all-skipped and failed cases returned nonzero. No Android rebuild or tablet interaction. Application source, Gradle/CI build configuration, tests and diagnostic harnesses are unchanged from M01.
