# M03 — isolated Android verification gate, not accepted

Branch `feature/m03-document-engine`. Tested application commit `d8a804a15e2e123cca26eec6ef54854e4fada3b0`, after pen-down checkpoint `8195bdef36c62efb79a794ed801c4405f607d4f7` and interrupted checkpoint `e7f0fe3b761502c5fbe24bb43b35fb0ea7cd1237`. ADR-013 and its six clarifications remain approved; no new architecture decision. For a later HEAD verify ancestry and inspect the diff before reusing evidence.

[Latest report](../reports/LATEST.md): host core, fault suite, mock/Mesa and Android build results, open pen-up regression, memory and scale limitations. APKs are built, Android tests are not run. Next action requires authorization naming the isolated device and allowed install/run; follow [tablet procedure](M03_TABLET.md). Do not install over owner data, clear, merge, push, start M04 or declare acceptance. Keep `captures/m02-accepted-8b5c24d/`, both root M02 presentation captures and interrupted patch intact.
