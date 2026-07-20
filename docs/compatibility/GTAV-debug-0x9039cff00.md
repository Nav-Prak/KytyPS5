# Debug handoff: `0x9039cff00` device-lost (post-blocker-49)

Session date: 2026-07-19. Written at session limit; this is the resume point.

## One-line state

GTA V reaches the prologue and compiles 78+ shaders, then **`vkWaitForFences` returns
`VK_ERROR_DEVICE_LOST`** while executing compute shader `0x9039cff00` (a decoupled-lookback
prefix-scan wave64 kernel). This is the documented blocker-42–49 terminator, still failing after
every fix in that chain. Fatal site: `context.cpp` `EXIT_NOT_IMPLEMENTED(result != eSuccess)`
after the fence wait (reported line 445; line numbers drift vs the running binary).

## Confirmed this session

- **Build is current.** Binary `_Build/windows/kyty_emulator.exe` mtime is newer than every
  recompiler/emitter source. Branch `feature/gtav-upstream-bd9086e` (Vulkan-Hpp merge). The
  blocker 42–49 work is in the **uncommitted working tree** (17 modified files incl.
  `shaderSubgroup.cpp`, `SpirvEmitter.cpp`, `spirvEmitter/*`, `ShaderIR.h`, tests). So all those
  fixes ARE built in.
- **`0x9039cff00` in this run:** 492 dwords / 289 instructions decoded, 71 CFG blocks + 3 loops,
  structurized to **73 blocks (no dispatcher fallback)**, emitted **29,280 SPIR-V words**,
  pipeline created successfully, device lost at queue 0 submission 47052, `DispatchDirect(9,1,1)`
  mode 0x41. Ran as **wave64 via single-wave workgroup emulation** (log: "Vulkan running 1
  64-lane compute wave through isolated workgroup cross-lane emulation (host subgroup=32)").
- Every blocker 42–49 fix is active for it: native 64-bit atomic swap, GLC coherent uint64
  acquire loads, ordered atomics, wave64 dispatch-initiator decode, single-wave workgroup
  emulation, loop-header split, **branch-free ballot**.

## RESOLVED via SPIR-V dump (2026-07-19): NOT a barrier deadlock — an infinite lookback poll

Dumped `_Shaders/3443_0078_new_shader_cs_00000009039cff00.spvasm` (511 KB) with
`--graphics-debug-dump true` and analyzed it. **The barrier-deadlock hypothesis below is
REFUTED.** Findings:

- 116 `OpControlBarrier`, 3 loops, 74 selections. 39 barriers are inside loop bodies.
- **Control flow is scalar/EXEC-driven and uniform (GCN-faithful).** Loop exit conditions come
  from `scc`/`vcc` computed via workgroup ballots (post-barrier shared scratch) or from scalar
  registers — the same value in all 64 invocations. E.g. loop 1 exits on `s13 < s3` (SGPRs);
  loop 3 exits on a ballot of `v9==0 && active`. So all invocations branch together and every
  barrier is reached by all 64 → **converged, not deadlocked.**
- The ticket `OpAtomicIAdd` (partition allocator) IS EXEC-guarded (`%345` = this lane's exec
  bit at line 449), so exactly one lane allocates — correct.
- **The terminator is an infinite poll.** Loop 3 (`%232`, the decoupled-lookback) reads a
  `uint64` from `buffers_uint64[2]` via acquire `OpAtomicLoad` (line 3457), gated by an
  `OpArrayLength` bounds check (`%2965`), and exits only when the polled state field (`v9`)
  becomes nonzero. Publish sites (`OpAtomicExchange`, lines 3072/4886) write the **same view/
  index space** `buffers_uint64[2]` with device-scope release (`0x48`); the poll uses device-
  scope acquire (`0x42`). Memory ordering and view are correct — no blocker-43/46 mismatch.
- **Therefore the poll never terminating means storage buffer #2 does not bind usefully at
  runtime:** if its descriptor is null-bound / window-degraded / wrong-sized, `OpArrayLength`
  returns 0 → the bounds check fails → the phi yields 0 → both publish and poll are dead → `v9`
  stays 0 forever → GPU TDR. Forward progress is not the issue (only 9 workgroups).

### Next diagnostic (highest value)

Determine buffer #2's runtime binding for this dispatch. It is one of 4 buffers
(BuildScalarProvenance descriptors=4, TrackResources buffers=4). The current log's
`GraphicsRenderDispatchDirect` per-buffer dump is gated on sampler/large-workgroup, so this
64-thread shader isn't logged. Add targeted logging in `descriptors.cpp` `NativeStorageBuffer`
(or `BindDescriptors`) that, for `program.shader_hash == 0x9039cff00`, prints each buffer's
resolved address / size / range / null-or-degraded status. Then one run shows whether buffer #2
binds with a nonzero range. Candidates if it's null/zero-range:
- the publication buffer's V# resolves onto a GPU-owned/image tracker page and gets the
  read-only-prefix null-bind (blocker 29/36) — but it is WRITTEN (atomic), so check that path;
- it is a bindless/tableless window whose union degraded (the degraded-null warnings in this run
  are pc 0x850/0x86c = shader `0x605ac67400`, a DIFFERENT shader — confirm whether `0x9039cff00`'s
  buffer #2 also degrades);
- the uint64 aliased view's array length is computed wrong vs the dword view.

Do NOT touch the wave64 barrier/CFG machinery — it is correct for this shader.

### Run result (2026-07-19): buffers bind fine — null-binding REFUTED

`ScanBufferBind` output for four dispatches shows every buffer binds with a valid nonzero range:
- buf[0]: ticket counter — stride 4, records 1, atomic, `range=0x4`.
- buf[1]: dword view of the payload region (base `..6100`), stride 4, records vary.
- **buf[2]: the publication pairs array — stride 8 (uint64), atomic, base `..6708` (= buf[0]+8),
  records = partition count (6/1/9/41 across dispatches), `range` = records*8 + 8 remainder
  (0x38/0x10/0x50/0x150). Binds correctly; `OpArrayLength` is nonzero; the poll's bounds check
  passes.**
- buf[3]: bindless payload (base `..6100`), stride 0, records large (payload elements).

So the infinite poll is NOT a zero-range/null binding. Verified further in the dumped SPIR-V that
the whole scan is structurally correct: poll (`OpAtomicLoad`, line 3457) and publish
(`OpAtomicExchange`, lines 3072/4886) use the **same** `buffers_uint64[2]` view + index space +
`OpArrayLength` bounds; both are EXEC-guarded (poll via a post-load exec-select; publish via an
outer exec-active branch `%2588` plus inner align/bounds `%2638`); device-scope release (`0x48`)
on publish, acquire (`0x42`) on poll. Everything checks out.

### Remaining cause (narrowed): runtime value of the cross-lane scan, not binding/ordering

The poll spins because the **published state at the polled slot never becomes the terminating
value** even though the mechanics are correct. Two candidates, both runtime-value-level (invisible
to static analysis):
1. **The wave64 workgroup cross-lane VALUE emulation is wrong for a reduction/scan step** — the
   barriers/control-flow are converged and correct (verified), but the actual shuffle/ballot/
   reduction *results* used to compute each partition's aggregate/state could be miscomputed, so
   the scan never reaches state 2 (PREFIX_AVAILABLE) and followers poll forever. This is the prime
   suspect: NVIDIA runs decoupled-lookback (CUB DeviceScan) fine natively, so the algorithm isn't
   the problem — our 64-lane-via-2x32-subgroup emulation of the cross-lane math is.
2. Inter-workgroup forward progress (less likely — NVIDIA handles decoupled-lookback, and
   dispatches here are small: 6/9/41 workgroups, co-resident).

### Next diagnostic (distinguishes the two)

Get runtime VALUE visibility. Options:
- Enable `--spirv-debug-printf true` and add a targeted printf in the emitted poll/publish to log
  (polled partition, state read) and (published partition, state written). One run shows whether
  producers ever publish a nonzero/terminating state and whether the polled slots match — this
  separates "cross-lane math produces wrong state" (candidate 1) from a slot/index mismatch.
- Or add a host readback of buf[2] contents after the timed-out dispatch.
- Or write a Vulkan compute regression that runs a small known decoupled-lookback-style kernel
  through the wave64 workgroup path and checks the cross-lane reduction result numerically — this
  validates candidate 1 fully offline (no game run).

The offline cross-lane-reduction regression is the highest-value non-game-run step: if the wave64
workgroup shuffle/ballot/reduce produces a wrong value for a known input, that IS the bug.

### FOUND (2026-07-19): `row_bcast15`/`row_bcast31` DPP emulated as identity (blocker 50)

Pursuing candidate 1 by auditing the cross-lane value path (not the game) found a concrete instance
of it. `EmitDppTargetLane` (`spirvEmitterValues.cpp`) handled quad-perm, row shift/rotate, and the
two mirror controls, but **fell through to the identity `return {subid, ...}` for `row_bcast15`
(dpp_ctrl `0x142`) and `row_bcast31` (`0x143`)** — even though the decoder passes the full nine-bit
dpp_ctrl through unvalidated, so those controls reach the emitter and silently self-shuffle. These
two are the **final cross-row combine stages of the canonical wave64 reduction**: after four
`row_shr` steps reduce each 16-lane row, `row_bcast15` carries lane 15 → lanes 16-31 and lane 47 →
lanes 48-63, then `row_bcast31` carries lane 31 → lanes 32-63. Emulated as identity, the reduction
adds each partial to itself instead of the neighboring row → **wrong per-partition aggregate**,
which is exactly candidate 1's "the scan never reaches state 2, followers poll forever."

Fix: added `EmitDppRowBroadcastTargetLane`; `0x142` → `(lane & 0x10) ? ((lane & 0x20) | 15) : lane`,
`0x143` → `(lane >= 32) ? 31 : lane`. All lanes have a valid in-bounds source so the fetch stays
active. Validated by two new Vulkan-executed wave64 regressions (`DppRowBroadcast15Wave64`,
`DppRowBroadcast31Wave64`) that run 64-thread dispatches through the isolated workgroup-scratch path
(`guest_wave64_scratch` + `OpControlBarrier`, `forbidden: OpGroupNonUniformShuffle`) and check the
exact per-lane broadcast; expected values come from the ISA, not the implementation. Full
`shader_recompiler_compute_tests` passes.

**Still pending in-game confirmation** that `0x9039cff00` actually uses `row_bcast` and that this
clears submission-44572 device loss. Not yet proven to be the sole terminator — it is a proven bug
in the same reduction path. To confirm the shader uses it, dump the decoded RDNA2
(`--shader-log-direction File`, `DumpShaderRecompilerOriginal`) and grep for `row_bcast`. If a
retest still hangs, the next candidate is a different cross-lane primitive (`ds_swizzle`,
`v_permlane`, or a `readlane`-based reduction) or candidate 2 (forward progress).

### Diagnostic added (built, ran — served its purpose; can be removed later)

Added targeted logging in `descriptors.cpp` `BindDescriptors` (the buffer loop): for
`program.shader_hash == 0x9039cff00`, logs each buffer's `bindless/written/read/atomic`,
resolved `desc_base/stride/records`, and the bound `range/offset` (line prefix
`ScanBufferBind 0x9039cff00:`), capped at 32 lines, via `LOGF`. It is NOT gated on
`--graphics-debug-dump`, so a normal `--printf-direction File` run captures it, and it logs
before the failing submit. Next run (no need for the slow `--graphics-debug-dump`):

```
& ".\_Build\windows\kyty_emulator.exe" `
  --game "D:\Games\PS5Games\PPSA04264[01.005.000]" `
  --file-read-min-latency-us 0 --vulkan-validation false --shader-validation false `
  --shader-optimization-type Performance --shader-log-direction Silent `
  --command-buffer-dump false --graphics-debug-dump false `
  --printf-direction File --printf-output-file "..\gtav-work\_gtav.log" `
  --profiler-direction None --spirv-debug-printf false --cs-skip-unresolved true
```

Then: `grep -a "ScanBufferBind" ..\gtav-work\_gtav.log`. Read buf[2]'s `bound_range` — if it is
`0x0` (or tiny), that confirms the publication buffer isn't binding and pinpoints which resolution
path (bindless/window/image-alias/read-only-prefix) killed it. This diagnostic is temporary; remove
it once the root cause is fixed.

## (Refuted) Primary hypothesis: workgroup barrier deadlock in divergent control flow

The wave64-on-subgroup-32 emulation implements cross-lane ops with **workgroup
`OpControlBarrier`** (`EmitGuestWaveWorkgroupBarrier`, `spirvEmitterValues.cpp:61`). One
`V_READFIRSTLANE` emits `EmitWaveBallot` + `EmitWaveShuffleU32` = **three workgroup barriers**
(`spirvEmitterFlowOps.cpp:257`; ballot at `spirvEmitterValues.cpp:125`, shuffle at `:101`).

A workgroup barrier requires **all 64 invocations (both native 32-lane subgroups) to reach it**.
Blocker 49 explicitly found a **ballot inside a loop-header block** in this exact shader — it
fixed that barrier's *SPIR-V validity* (made it branch-free) but **not its runtime convergence**.
If the loop diverges across the two native subgroups, the barrier deadlocks → GPU TDR →
`DEVICE_LOST`. Validation is off in-game, so a deadlocking-but-valid module isn't caught. This
matches the signature: valid SPIR-V, pipeline OK, hang only on execute.

**Open question the dump must answer:** are this shader's loop/branch conditions wave-uniform
(both native subgroups take the same path → barrier safe) or per-lane divergent (→ deadlock)?
Need to see `OpControlBarrier` placement relative to `OpLoopMerge`/`OpSelectionMerge` in the
actual emitted SPIR-V.

## Why pure-offline extraction failed (don't retry it for this shader)

`0x9039cff00` is **runtime-loaded from GTA's RPF archives**, not cleanly baked in the ELF:

- Module base = `0x900000000` (cross-checked: import caller `0x9029667dd` = base + module
  offset `0x029667dd`). Shader vaddr = `0x39cff00`.
- ELF `gtav-work/gtav-eboot.elf`: PT_LOAD `ph[7]` (vaddr `0x3930000`, foff `0x3934000`, delta
  `0x4000`) contains it → file offset `0x39d3f00`.
- BUT `0x39d3f00` lands **416 bytes inside** a 463-dword baked shader (bracketing `s_endpgm` at
  vaddr `0x39cfd5c` and `0x39d0498`). No 492-dword shader starts at `0x39cff00`. The static
  bytes there are a *different, adjacent* baked shader.
- ELF has 347 baked shaders (aligned `0xbf810000` count), clustered in file `0x3900000–0x3bff000`
  (ph[7]/ph[8]); only 2 carry the `0xBEEB03FF` magic. So most static shaders are raw streams.

Conclusion: static-ELF extraction cannot get this runtime shader's bytes.

## Chosen next step (user picked this): one dump run

Get the exact emitted SPIR-V via the emulator's built-in dump. **IMPORTANT flag correction:**
the `.spv`/`.spvasm` dump is gated on `Config::GraphicsDebugDumpEnabled()`
(`DumpShaderRecompilerSpirv`, `shader.cpp:1336`) — i.e. **`--graphics-debug-dump true`**, NOT
`--shader-log-direction File`. (`--shader-log-direction File/Console` only adds the decoded-RDNA2
text + IR via `DumpShaderRecompilerOriginal`, useful for reconstructing bytes for a replay
harness.)

Run command (add `--graphics-debug-dump true`; keep everything else the same):

```
& ".\_Build\windows\kyty_emulator.exe" `
  --game "D:\Games\PS5Games\PPSA04264[01.005.000]" `
  --file-read-min-latency-us 0 --vulkan-validation false --shader-validation false `
  --shader-optimization-type Performance --shader-log-direction Console `
  --command-buffer-dump false --graphics-debug-dump true `
  --printf-direction File --printf-output-file "..\gtav-work\_gtav.log" `
  --profiler-direction None --spirv-debug-printf false --cs-skip-unresolved true
```

Output: `KytyPS5/_Shaders/NNNN_MMMM_new_shader_cs_000000009039cff00.spvasm` (and `.spv`). Find it
with: `ls _Shaders | grep 9039cff00`. The shader compiles before the crash, so it WILL be dumped.
(This dumps ~78 shaders and slows the run; acceptable for one diagnostic pass.)

## What to do with the dump (next session)

1. Open the `..._cs_..9039cff00.spvasm`. Check every `OpControlBarrier`: is it inside an
   `OpLoopMerge` region whose back-edge/exit condition is per-lane (divergent) rather than
   wave-uniform? Confirm/refute the deadlock. `OpControlBarrier` count should be ~3× the
   read-first-lane/ballot/shuffle sites.
2. If confirmed: design a convergence fix. Options (do NOT implement speculatively — the
   bring-up doc forbids touching wave64 machinery without evidence):
   - (a) Route wave64 compute with cross-lane ops in divergent flow to a fully-predicated
     (flattened-EXEC) model so all invocations execute all blocks and barriers stay converged
     (how real GCN wave64 behaves). Heavy but principled.
   - (b) Make every branch that could split the two native subgroups wave-uniform (ballot the
     full 64-lane condition before branching) so they never diverge at a barrier.
3. Then seed the **offline replay harness** from the dump's decoded-RDNA2 bytes (a `--replay`
   mode in `shader_recompiler_compute_tests` that compiles the bytes as wave64 compute with
   `threads_num={64,1,1}` — see `ShaderRecompiler.cpp:802` for the `workgroup_wave64_waves=1`
   trigger) to iterate the fix without repeated game runs.

## Key file references

- Workgroup barrier: `spirvEmitterValues.cpp:61` `EmitGuestWaveWorkgroupBarrier`; used by
  `EmitWaveShuffleU32` (:101), `EmitWaveBallot` (:125), and `EmitSubgroupLocalInvocationId` (:499).
- `EmitReadFirstLaneU32`: `spirvEmitterFlowOps.cpp:257` (ballot + shuffle = 3 barriers).
- `workgroup_wave64_waves` set: `ShaderRecompiler.cpp:802–814` (stage=Compute, wave_size=64,
  threads∈[64,1024] multiple of 64; ==64 → 1 wave). Emitter copy: `SpirvEmitter.cpp:453`.
- Fence-wait fatal: `context.cpp` `EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess)` after
  `vkWaitForFences` (there are ~14 such guards; this is the fence one — do NOT treat the others
  as features).
- SPIR-V dump: `shader.cpp:1336` `DumpShaderRecompilerSpirv` (needs `--graphics-debug-dump true`),
  folder from `Config::GetShaderLogFolder()` default `_Shaders`.
- Full history: `docs/compatibility/GTAV-bringup-plan.md` blockers 1–49 (this shader is 42–49).
  Forward plan: `docs/compatibility/GTAV-forward-plan.md`.

## Do-not-regress reminders (from the bring-up doc's blocker-49 open items)

Do not: restore helper-generated selection blocks in the workgroup ballot; split the 64-bit
atomic into two 32-bit ops; weaken atomicity; broaden the qword-load matcher; force either kernel
to wave32; admit conditional multi-wave barriers; relax texture-cache ownership — all without new
evidence. Capability backlog (deferred, do not approximate): `VOP3 0x14c` native FP64 FMA, two
`MIMG 0xe6` BVH variants.
