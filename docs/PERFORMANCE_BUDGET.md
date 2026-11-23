# Performance targets (proposed, unverified)
Reference hardware: Samsung Galaxy Tab S10 FE+ / Xclipse 540 / 90 Hz as described in historical documentation. Nominal refresh interval 11.11 ms. Record p50/p95/p99 presentation frame intervals; never equate callback duration to stylus-to-photon latency.

Goals: event-driven idle; bounded memory and upload bytes; visible-complexity-driven rendering; no missing strokes; smooth practical zoom; test 1M stored strokes including overlapping adversarial scene. Measure before setting hard latency gates.
