# VIOS checked 24-bit benchmark snapshot

The 2026-10-16 Agon Light 2 measurements support selecting **DIRECT** as the current default. At 100 iterations, direct threading used **11.3–14.1% less elapsed time** than token threading across the five measured kernels. Both assembly engines and the standard-C reference remain available. These results compare the implemented checked/profiled engines; they do not establish a universal advantage for direct threading.

The table below is calculated by `tools/analyze_vios_bench.py` from the [verified raw transcript](../bring-up/vios-framed-verified.bench.txt), using each condition's median of nine samples. Engine IDs are C=0, TOKEN=1 and DIRECT=2.

| Workload, 100 iterations | C ticks | Token ticks | Direct ticks | Token ms | Direct ms | Direct time reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Arithmetic / bitwise | 7,731 | 2,426 | 2,118 | 8.424 | 7.354 | 12.7% |
| Conditional branches | 8,159 | 2,332 | 2,003 | 8.097 | 6.955 | 14.1% |
| Colon calls | 5,134 | 1,712 | 1,474 | 5.944 | 5.118 | 13.9% |
| Loop / index / addition | 3,301 | 1,169 | 1,037 | 4.059 | 3.601 | 11.3% |
| Cell memory access | 8,210 | 2,278 | 1,968 | 7.910 | 6.833 | 13.6% |

Reduction is `100 × (token − direct) / token`. Direct execution was approximately 3.18–4.17 times as fast as the C reference in this snapshot. Timer readings use the configured nominal frequency; no external oscillator calibration is claimed.

## Measurement profile

The target's benchmark identifies itself as `checked24+peaks`. Both assembly engines execute the same bounds/budget checks and update data/return high-water counters on successful pushes. IP and the data pointer are cached; TOS remains memory resident. Ordinary arithmetic, calls, branches, loops and memory operations stay inside assembly, while complex arithmetic and console/dynamic services use explicit C traps. This is the initial reliable profiling configuration, not a maximally optimized release engine.

Timer 0 runs from the nominal 18.432 MHz system clock divided by 64: **288,000 ticks/s**, or **3.472222 µs/tick**. Its 16-bit wrap period is 227.556 ms. Each measured interval excludes serial output, and results are validated after the finishing timestamp. See the [timer contract](../research/vios-timer.md) for register configuration and wrap limitations.

The run contains 45 conditions: three engines × five kernels × iteration counts 25, 50 and 100, with nine samples each. The analyzer checks sample counts, reported medians, agreement of final results at 100 iterations, and approximately affine timing growth across the three batch sizes. All checks passed. The largest sample was 8,210 ticks, about 28.507 ms, about one eighth of the wrap period. Scaling checks and conservative intervals reduce the risk of wrap aliasing; endpoint timer reads alone cannot detect multiple wraps.

The transcript separately records a median empty timestamp pair of zero ticks and empty-word execution of 33 ticks per engine. The table reports raw workload medians without subtracting either measurement. It also records **903 ticks (3.135 ms)** to compile a 39-character definition, emitting both code formats together. That is one resident dual-emitter compile sample, not a comparison of separate token/direct compiler times.

After that run's setup and earlier language checks, code-arena usage was **689 token bytes and 1,692 direct bytes**. These are dictionary-state totals, not isolated kernel sizes or the complete image. Keeping both arenas, source and the C reference costs memory; direct threading's timing advantage here does not imply better code density. The [artifact manifest](../../artifacts/vios-2026-10-16/manifest.json) supplies measured complete-image sizes/hashes.

## Reproduce and interpret

Run on the wired board with the existing toolchains and Python environment:

```sh
make vios esp32-probes test
.venv/bin/python tools/vios_hardware.py --log docs/bring-up/my-vios-benchmark.jsonl
.venv/bin/python tools/analyze_vios_bench.py docs/bring-up/my-vios-benchmark.bench.txt --output /tmp/vios-benchmark.json
```

The deployment creates a fresh RAM session and verifies readback before running correctness checks and `BENCH`. For an existing persistent session, use the harness's `benchmark` tool instead; see the [harness runbook](../bring-up/vios-harness.md). Do not run competing serial clients.

The [current analyzed sample set](vios-profiled.json) corresponds to the framed verified transcript above. The earlier [initial profiled analysis](vios-profiled-initial.json) remains as separate evidence; its raw sequence differs slightly, including a one-tick memory median difference at 100 iterations. Recompute comparisons from the named transcript rather than mixing sample sets. Historical evidence identities remain preserved.

These kernels measure dispatch-heavy computation, calls, control flow and cell memory. They do not measure VDP graphics/audio, SD I/O, interactive keyboard services, large applications, cold boot or future CPU ports. Further optimizations should be compared with the same known-result workloads, profile settings, repeated samples and memory accounting.

Repeated BENCH calls currently publish another BM-COMPILE definition, consuming one dictionary slot plus source/code each time. Redeploy a fresh RAM image for comparable full benchmark runs. This profiling utility does not reclaim those definitions.
