# M04 backend selection — compact evidence digest

Archive SHA-256: `6e47f1ff46c14be8b1102e063ba954bc0ea7ed41f2b252aca43febb6e1c3c05f`. Analyzed uploaded backend-selection-02.zip; archive CRC check passed. Local source checkpoint reported by owner: 8f9b530 (not independently matched to APK). Tested APK SHA-256 reported by owner: 78f76a765cb3f125351692221e0b15e0d6988d21deaf1c093cefb66bea502b1d.

- 115/115 successful receipts; 10 parity runs; 105,000 measured frames; 9,375 GPU samples (all expected); all measured presentation timestamps positive. All 115 pre-run thermal statuses 0. Fixture hashes consistent by scene and all identities report 1800x2880.
- Independently recomputed raw measured-frame wall/CPU/GPU/presentation distributions; all match stored summaries.
- 104,895 consecutive positive presentation gaps; aggregate observed rate 90.096 Hz; 0 estimated missed refreshes using campaign rounding rule (not independent FrameTimeline verification).
- All 5 modes byte-identical within each of the two parity fixtures, including reversed paint order. Pixel SHA-256: parity: 5aeb8eb66425649df587d13968b431850309766758cfc7598fbbd73af04eecb5; reverse: eff6fdcbc639c6f10bbcb049693824b4c74aa6ef9297f00e60e13dd800ce920d.
- Upload counters: resident pan/dense 0; live 216 B/chord expanded, 28 B/chord instanced. No claim of actual allocation/transfer traffic equivalence.

## Timings
Median of 3 per-run statistics with timer=1; milliseconds. Column order wall p50/p95/p99, thread CPU p50, GPU p50, tick-to-present p50.

|Scene|Mode|Wall p50|p95|p99|CPU p50|GPU p50|Tick-present p50|
|---|---|---:|---:|---:|---:|---:|---:|
|pan|g2|3.128|4.283|4.866|2.932|0.805|33.307|
|pan|g3e|3.055|4.177|4.759|2.867|0.802|33.309|
|pan|g3i|3.215|4.343|4.864|3.046|0.847|33.308|
|pan|vke|2.016|2.953|3.405|1.706|0.691|33.309|
|pan|vki|2.081|2.924|3.280|1.730|0.696|33.309|
|live|g2|3.226|4.330|4.914|3.026|0.879|33.310|
|live|g3e|3.146|4.325|4.832|2.973|0.871|33.307|
|live|g3i|3.178|4.378|5.029|3.007|0.912|33.309|
|live|vke|2.075|2.998|3.390|1.763|0.782|33.311|
|live|vki|2.022|2.908|3.478|1.713|0.787|33.306|
|dense2|g2|2.426|3.411|3.885|2.192|0.487|33.310|
|dense2|g3e|2.386|3.353|3.759|2.148|0.482|33.307|
|dense2|g3i|2.408|3.338|3.782|2.181|0.485|33.311|
|dense2|vke|1.926|2.875|3.290|1.608|0.495|33.311|
|dense2|vki|1.879|2.854|3.227|1.536|0.492|33.307|
|dense16|g2|2.608|3.690|4.168|2.434|0.543|33.307|
|dense16|g3e|2.569|3.611|4.094|2.393|0.549|33.310|
|dense16|g3i|2.607|3.687|4.137|2.426|0.558|33.311|
|dense16|vke|1.974|2.935|3.330|1.636|0.528|33.312|
|dense16|vki|1.979|2.878|3.286|1.676|0.538|33.311|
|dense64|g2|3.192|4.321|4.752|3.010|0.766|33.312|
|dense64|g3e|3.137|4.246|4.705|2.941|0.770|33.310|
|dense64|g3i|3.190|4.250|4.831|2.960|0.809|33.309|
|dense64|vke|2.065|2.946|3.378|1.752|0.673|33.311|
|dense64|vki|2.107|3.027|3.451|1.759|0.683|33.305|

|Scene|VKI vs G3I wall reduction|CPU reduction|GPU reduction|
|---|---:|---:|---:|
|pan|35.3%|43.2%|17.9%|
|live|36.4%|43.0%|13.7%|
|dense2|21.9%|29.6%|-1.4%|
|dense16|24.1%|30.9%|3.6%|
|dense64|33.9%|40.6%|15.5%|

## Overhead, memory and presentation
|Mode|Pan wall p50 t=-1 / 0 / 1 (ms)|Worst run p99 present gap (ms)|End PSS median (MiB)|Init median (ms)|
|---|---|---:|---:|---:|
|g2|3.138 / 3.141 / 3.128|11.571|46.36|43.57|
|g3e|3.129 / 3.183 / 3.055|11.559|46.38|44.22|
|g3i|3.169 / 3.154 / 3.215|11.544|46.35|43.66|
|vke|1.939 / 1.998 / 2.016|11.536|49.82|51.28|
|vki|1.963 / 2.065 / 2.081|11.514|49.75|52.03|

## Decision and limits
Native Vulkan with compact geometry is the leading candidate: lower render-thread CPU and wall time across five workloads; same tested pixels; 87% fewer geometry bytes per chord. No presentation latency improvement established (~33.3 ms all modes). GLES reports 2 draws per stroke; Vulkan 3; inspect actual operations before attributing gains solely to API. Small synthetic scenes, no maximum capacity test, no uninterrupted sustained stress or lifecycle recovery proof, no peak GPU/process residency proof, no independent FrameTimeline join. Pre-run thermal status does not establish in-run thermal stability. Instrumentation overhead budget not established (VKI pan medians ~6% wall difference between t=-1 and t=1, not causal estimate). No production migration approved. Preserve architecture and existing raw evidence; only targeted remaining checks.
