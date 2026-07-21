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

### UPDATE (2026-07-19): this shader does NOT use row_bcast — candidate 1 REFUTED for it

The decoded RDNA2 is already on disk at
`_Shaders/original/3443_0078_new_shader_cs_00000009039cff00.rdna2` (and the `1835_0079` copy). It
contains **zero `row_bcast`**. Its wave64 reduction (two copies, at PC `0x204` and `0x310`) is:

```
v_add_nc_u32 v20, v20.dpp(ctrl=0x111), v20   ; row_shr:1  \
v_add_nc_u32 v20, v20.dpp(ctrl=0x112), v20   ; row_shr:2   > inclusive scan within each 16-lane row
v_add_nc_u32 v20, v20.dpp(ctrl=0x114), v20   ; row_shr:4   > lanes 15/31/47/63 = their row sums
v_add_nc_u32 v20, v20.dpp(ctrl=0x118), v20   ; row_shr:8  /
v_permlanex16_b32 v19, v20, -1, -1           ; each half reads lane 15/31 of the OTHER half
v_add_nc_u32 v20, v20.dpp(ctrl=0xe4), v19    ; lanes 15,31 = sum(0..31); 47,63 = sum(32..63)
s_bfm_b64 exec_lo, 32, 32                     ; exec = lanes 32..63 only
v_readlane_b32 s0, v20, 31                    ; s0 = sum(0..31)
v_add_nc_u32 v20, s0, v20                      ; lanes 32..63 += sum(0..31) => lane 63 = sum(0..63)
s_mov_b64 exec_lo, vcc_lo                      ; restore exec
```

So the row_bcast fix (blocker 50), while a real bug, is **not this shader's terminator**. The
reduction instead relies on `v_permlanex16_b32`, `v_readlane_b32` on lanes 31/63, and an
`s_bfm_b64` exec split. Each was audited on the wave64 workgroup path:

- **`EmitReadLaneU32` / `EmitPermlaneB32`** both call `EmitWaveShuffleU32(value, guest_lane)`, which
  in wave64 mode reads the 64-lane workgroup scratch, so lanes 32-63 correctly read a source lane in
  subgroup 0. `permlanex16 -1,-1` computes `target = (row ^ 16) | 15`, i.e. each 16-lane half reads
  lane 15 of the other half; for the reduction only lanes 15/31/47/63 matter and those are identical
  under swap or broadcast semantics.
- **`s_bfm_b64 exec_lo, 32, 32`** lowers through `EmitBitFieldMaskU64` to `exec_lo=0,
  exec_hi=0xFFFFFFFF` (lanes 32-63). Compute forces `lane_mask_mode=NativeWave`
  (`shaderSubgroup.cpp:37`), so the emitter runs in `exact_subgroup_operations` mode where
  `EmitMaskActiveBool` tests `EmitLaneMaskPartU32` by **guest** lane 0-63 — lane 40 checks `exec_hi`
  bit 8, lane 10 checks `exec_lo` bit 10. The masked `v_add` therefore applies only to lanes 32-63.

Tracing the whole sequence with per-lane value = lane index yields lane 63 = 2016 = sum(0..63), the
correct wave sum. A new Vulkan wave64 regression `Permlanex16Wave64BroadcastsOtherHalf` confirms the
one primitive that previously lacked direct wave64 coverage (permlanex16) produces the exact
per-lane broadcast on the workgroup-scratch path. **So the cross-lane VALUE emulation for this
shader's reduction is correct; candidate 1 is refuted for `0x9039cff00`.**

### Remaining candidates after candidate-1 refutation (need runtime value visibility)

The device is lost even at `DispatchDirect(2,1,1)` — two workgroups trivially co-reside, so this is
not a many-workgroup occupancy/forward-progress failure. With the aggregate proven correct, the
open candidates are all runtime-value-level and need a game run to distinguish:

1. **State-field / publish or ticket-init, not the aggregate.** The published qword is
   `{state, aggregate}` via `buffer_atomic_swap_x2 v9,v19` where `state = (partition>0) ? 1 : 2`.
   Partition 0 publishes state 2 (inclusive) immediately. If the ticket counter (buf[0]) or the
   state/value pairs (buf[2]) are not zero-initialized at dispatch start, a follower can read a
   stale state or a wrong partition index and never converge. Verify the guest clears these before
   the scan and that the emulator preserves that clear.
2. **Cross-workgroup visibility of the publish on this host.** Publish is device-scope release
   `OpAtomicExchange` (`0x48`) and poll is device-scope acquire `OpAtomicLoad` (`0x42`) on the same
   `Coherent` uint64 view (blocker 45/46). If the host driver does not make the exchange visible to
   another workgroup's acquire load despite device scope, the poll spins. This is host/driver
   behavior, hard to prove offline.

**Highest-value next diagnostic (requires one game run):** enable `--spirv-debug-printf true` and
log `(polled_partition, state_read)` at the poll and `(published_partition, state_written)` at the
publish, OR read back buf[2] after the timed-out dispatch. That directly shows whether producers
ever publish a terminating state and whether the polled slots match — separating candidate 1-residue
(some other wrong value) from a visibility/initialization problem. Do NOT change the wave64 machinery
further without that evidence; it is now audited and test-covered.

## Run of 2026-07-19 (80 shaders, submit_seq 39690) — mechanics all verified correct

The retest carrying the blocker-50 row_bcast fix still lost the device, as expected: this shader has
no `row_bcast`. New facts from that log and the matching 29,280-word SPIR-V dump:

- **The shader is dispatched exactly ONCE and hangs on that first dispatch.** `grep -c` on the
  submit pattern returns 2 only because the `vkWaitForFences failed` line repeats the same `args=`
  text; there is a single `vkQueueSubmit`, `DispatchDirect(6,1,1)`, `submit_seq=39690`, queue 0.
  Cross-dispatch drift of the ticket counter is therefore **not** the mechanism in this run.
- **The `Vulkan running N 64-lane compute wave...` line is rate-limited to the first 16 emissions**
  (`shaders.cpp` `log_count < 16`), and exactly 16 appear. Its absence for a given shader proves
  nothing. `0x9039cff00` must be on `WorkgroupWave64` anyway: `ConfigureShaderSubgroup` would have
  hit the `Unsupported` `EXIT` for a 64-wide compute program that needs exact subgroup ops, and no
  such exit occurred.
- **The 256-byte bind-remainder is correct on the qword view.** buf[2] reports
  `desc_base=0x60a5be6708` but `offset=0x6700`, i.e. remainder 8. `EmitMemoryByteAddress` adds that
  remainder for both the publish (`EmitAtomicSwapU64`) and the poll (`EmitAtomicQwordAcquireLoad`),
  so both compute `qword_index = (8 + p*8) >> 3 = 1 + p` — consistent, 8-aligned, and inside the
  `OpArrayLength` of `0x38/8 = 7` for partitions 0..5. buf[0]'s ticket counter has remainder 0.
- **The poll loop is structurally correct in the emitted SPIR-V.** Loop header `%227` re-loads `v9`
  (`%2862`), recomputes the per-lane predicate `v9 == 0 && exec_active` (`%2864`), runs the
  branch-free workgroup ballot (`OpAtomicAnd` / barrier / `OpAtomicOr` / barrier / load / barrier)
  into `vcc`, derives `scc`, and exits on `%2905`. The three barriers live in the loop header and so
  re-execute every iteration under uniform control flow. Inside the body the atomic load is guarded
  only by `aligned && in_range` (`%2966`); the write-back to `v9` is separately exec-masked, so
  exec-inactive lanes retain the `v9 = 1` set at guest PC `0x2b0` and never hold the loop open.
- `EmitDeviceAtomicMemoryBarrier` emits `OpMemoryBarrier`, not `OpControlBarrier`, so the
  publish's divergent (`exec = lane 0`) region contains no execution barrier — no deadlock there.

Every mechanical layer — reduction value, cross-lane primitives, exec masking, qword index, bounds,
memory ordering, loop structure, barrier placement — has now been verified. What has **never** been
observed is the **contents** of buf[0] and buf[2] at dispatch start. If the ticket counter does not
begin at 0, every workgroup takes a partition index past the 6-record array; publishes then fall
outside `OpArrayLength` and are dropped, and pollers read out-of-range slots whose phi default is
**zero** — which is exactly the loop's "keep spinning" value. That single unobserved input explains
the hang without contradicting any verified layer.

`descriptors.cpp` now dumps up to 12 leading guest dwords per buffer next to each `ScanBufferBind`
line (`buf[N] guest@0x... dwords: ...`). The CPU read enters the memory tracker's fault path, which
synchronizes native bytes, so it reports the authoritative contents. Read buf[0]'s single dword (the
ticket counter — expect 0) and buf[2]'s 12 dwords (six `{state, value}` pairs — expect all zero, i.e.
state INVALID, before the scan). A nonzero counter or pre-set states confirms the initialization
candidate and points at which clear the emulator is losing.

## ROOT CAUSE FOUND (2026-07-19, 78-shader run): the tile-state buffer is stale, not zero

The contents dump settled it:

```
buf[0] guest@0x60a5be6700 dwords: 00000000                          <- ticket counter: correct
buf[1] guest@0x60a5be6100 dwords: ffffffff x12                      <- payload/input region
buf[2] guest@0x60a5be6708 dwords: ffffffff ffffffff ffffffff ...    <- tile-state pairs: WRONG
buf[3] guest@0x60a5be6100 dwords: ffffffff x12                      <- payload bindless view
```

`buf[2]` is the decoupled-lookback tile-state / value array. The algorithm (Merrill & Garland
single-pass scan; the shader's own prologue has no self-init of its slot) **requires this buffer
pre-cleared to 0** so that state 0 = INVALID/not-yet-published. The poll loop
(`0x2cc`-`0x2f0`) waits *while* the polled state `== 0`, and the outer window-scan
(`0x2a8`-`0x358`) terminates **only** through `0x300 s_cbranch_scc1` when it finds a predecessor with
`state == 2` (INCLUSIVE). With every slot stale at `0xffffffff`:

- the poll never waits (`0xffffffff != 0`), so a follower reads garbage instead of waiting for the
  real publish;
- no predecessor ever reads as `state == 2`, so `0x300` never breaks out;
- `0x304 v_subrev_nc_u32 v19, 64, v19` walks the window backward forever.

That is the GPU infinite loop that loses the device. It also explains why the dispatch width
(2/3/6/9/41) and shader count never mattered: the very first workgroup that does any lookback spins.

**Why the counter is zero but the states are not.** Both live in the same 4 KB page
(`0x60a5be6000`), so this is not a whole-page allocation state: the counter's 4 bytes at
`+0x700` were specifically written to 0 while the rest of the page kept `0xffffffff`. So a targeted
clear zeroed the counter and the tile-state clear is being lost. The diagnostic reads guest memory
through the tracker fault path (which would pull a GPU-cleared native buffer back to guest), and it
still shows `0xffffffff` — so the clear is not reaching this guest address through either the CPU
guest-fill path or a coherent GPU buffer. Candidates: a `DMA_DATA` immediate fill that takes the
wrong `FillBuffer` path or hits a disjoint buffer-cache entry; or a compute/clear the emulator drops
(note `--cs-skip-unresolved` dropped three large `0x605ac…` passes this run, though none look
state-buffer-sized).

### Diagnostic added for the next run (find the missing clear)

- `bufferCache.cpp` `FillBuffer` logs every fill of <= 4 KB as
  `FillBufferTrace: vaddr=… size=… value=… path=cpu|gpu image_overlap=…`. `DMA_DATA` immediate
  clears route here (`graphicsRun.cpp` `DmaData` `src_sel==2`).
- `graphicsRun.cpp` `WriteData` logs small writes as `WriteDataTrace: dst=… dwords=… first=…`. The
  counter clear likely comes through here; the contrast shows which mechanism handles the counter
  versus the missing state clear.

After the next run: from `ScanBufferBind` note buf[2]'s guest address (e.g. `…6708`), then
`grep -aE "FillBufferTrace|WriteDataTrace" _gtav.log` and look for any fill/write whose range covers
that address with value 0.
- **If a `FillBufferTrace` covers it:** the clear IS issued; the bug is its `path`/coherence (a
  `gpu` path fill to a buffer-cache entry the scan doesn't share, or a `cpu` fill racing the bind).
  Fix the fill so it lands in the memory the scan uploads from.
- **If nothing covers it:** the clear is a compute dispatch (possibly skipped) or another packet.
  Next add per-dispatch output-range logging (or check the skip list against the state address).

Both traces are temporary scaffolding gated only by a line cap; remove with the `ScanBufferBind`
diagnostic once the clear path is fixed.

## Run of 2026-07-19 (78 shaders) — no CP op clears buf[2], and the cap masked the late writes

The retest confirmed buf[2] is still stale and produced two useful facts and one diagnostic flaw:

- buf[2] is at guest `0x60a5c3b308` this run (was `…6708`), `records=41`, still all `0xffffffff`
  across all 8 binds. buf[1]/buf[3] (payload, base `…c3b…`) are `0xffffffff` too — the whole region
  reads as a poison-like pattern.
- The three `FillBufferTrace` lines are unrelated 4-byte `value=1` fills; **no CP fill clears the
  state buffer.** The emulator does not poison-fill `0xffffffff` (grep confirms), so the pattern is
  genuine stale/prior-frame data.
- **Diagnostic flaw:** `WriteDataTrace` hit its 96-line cap during the first frames, but the scan
  binds only after 80 shaders compile, so the scan's own setup writes are past the cap and were
  never logged. The captured `WriteDataTrace` lines are early, unrelated writes to other addresses
  (`…31000`/`…45000` counter-reset clusters). So this run neither proves nor disproves a WRITE_DATA
  clear of buf[2] — the cap hid the relevant window.

A state-buffer clear via WRITE_DATA would be a burst of ~82 zero dwords (41 pairs) and games rarely
clear that way; a `DMA_DATA` fill would show in `FillBufferTrace` (none did). So the clear is most
likely a **compute dispatch** whose write to buf[2] is either dropped (skipped shader) or lands in a
buffer-cache entry the scan does not share (a coherence/ownership issue, blocker 18-36 territory).
My bind-time guest read cannot see a GPU clear that executes later, so the `0xffffffff` at bind time
is consistent with both "no clear" and "clear-but-incoherent".

### Diagnostic replaced: scan-diag write ring (`scanDiag.h` + `descriptors.cpp`)

The capped traces are replaced by an uncapped ring that records **every written buffer range from
every shader's `BindDescriptors`** (tag = shader hash) plus every CP `WriteData` (tag 1) and
`FillBuffer` (tag 2 = cpu path, 3 = gpu path). At the scan's buf[2] bind, `ScanDiagDumpOverlaps`
prints every ring entry whose range overlaps buf[2]'s enclosing 4 KB page:

```
ScanDiagOverlap: writer tag=0x… range=0x…..0x… value=0x… seq=…
```

Interpretation after the next run (`grep -aE "ScanBufferBind 0x9039cff00: buf\[2\]|ScanDiagOverlap"`):
- **A writer with `tag` != `0x9039cff00` and value 0** → that is the clear. If its `tag` is a shader
  hash, the clear is a compute dispatch that ran but is incoherent with the scan's read — fix the
  buffer-cache ownership so the scan uploads from the cleared bytes. If `tag` is 1/2/3, a CP op
  clears it but on the wrong path/buffer.
- **Only `tag == 0x9039cff00` writers (the scan's own publishes), or `no recorded … write
  overlaps`** → nothing clears buf[2] before the scan; the game relies on zero-initialized
  allocation that the emulator is not providing, and the fix is to zero this scan-scratch on
  allocation / first GPU use rather than to chase a clear packet.

The ring holds 8192 entries (plenty for within-frame correlation) and is temporary scaffolding.

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
