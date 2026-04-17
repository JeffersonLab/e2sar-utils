# tests/

| File | Purpose |
|------|---------|
| `test_loopback.sh` | End-to-end integration test: starts a receiver on loopback, runs the sender with parallel file streams, verifies all buffers were received, and reports PASS/FAIL. Supports `--toy` / `--gluex` schema selection. |
| `test_e2sar.cpp` | Minimal C++ smoke test that links against the E2SAR library, parses a dummy URI, and confirms the installation is working correctly. |
| `factored_gluex_analysis.C` | ROOT macro: reference implementation of GlueX kinematic-fit event processing used as the design basis for `GluexFileProcessor` and `GluexEventData`. Functionally equivalent to `gluex_event_selection.C`|
| `gluex_event_selection.C` | C reference implementation of glueX data analysis. ROOT macro: applies kinematic-fit quality cuts to GlueX events and fills Dalitz-plot histograms for offline analysis. |
| `compare_histos.C` | ROOT macro: loads a data file produced by `gluex_event_selection.C` and overlays pre/post-cut histograms for visual validation. |
| `read_dalitz_root.py` | Python reference implementation for reading Dalitz toy-MC ROOT files using PyROOT; useful for cross-checking the C++ serialization output. |
