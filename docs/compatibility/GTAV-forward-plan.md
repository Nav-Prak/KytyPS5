# GTA V prologue bring-up - forward implementation plan

## Context and current baseline

GTA V (PPSA04264) boots through platform initialization, reaches Story Mode, and drives RAGE's
GPU-driven bindless object pipeline on the isolated Vulkan-Hpp integration branch. The 2026-07-19
blocker-48 retest cleared the launch-time wave64 failure: the `32x32x1` shader now runs as sixteen
isolated guest waves. The game reached frame 1932 and 82 shader-compile messages before queue 1 lost
the device while executing `0x9039cff00`, the cross-workgroup prefix-scan kernel previously tracked
through blockers 42-47.

The fatal at `context.cpp:445` is a generic fence-result guard, not an unimplemented feature in that
file. The exact failing submit is queue 1 sequence 44572, `DispatchDirect(41, 1, 1)`, mode `0x41`.
The 289-instruction wave64 shader decoded into 71 CFG blocks with three loops/back edges and
structurized to 73 blocks. Its module, layout, and compute pipeline were created successfully; the
device was lost only after execution began.

Exact offline replay found blocker 49. The workgroup-wave64 ballot helper inserted conditional
scratch-clear and predicate-set blocks inside a guest loop header. That moved `OpLoopMerge` into a
continuation block while the guest back edge still targeted the original header, producing invalid
SPIR-V. With game validation disabled the driver accepted the module and later lost the device.
The helper now uses branch-free per-lane atomic clear and set operations separated by workgroup
barriers, so it adds no selection merge to guest control flow. The exact shader validates and
completes a synthetic 41-workgroup Vulkan dispatch. Permanent static and Vulkan regressions cover a
wave64 ballot in a loop header. Release builds and all eleven standalone tests pass; blocker 49 is
code-complete pending the next in-game run. All blocker 42-48 qword, CFG, ordering, wave-size, and
multi-wave fixes remain in place.

Current-log inventory:

- unresolved import calls: 0;
- one negative Vulkan result: `eErrorDeviceLost` at queue 1 submission 44572;
- `ReadConst` failures: 0;
- compute skips: three unique shaders: capability-gated `V_FMA_F64` and the two
  `IMAGE_BVH_INTERSECT_RAY` variants remain deferred;
- read-only buffer-owned image null aliases: 0, confirming blocker 36;
- 13 image/metadata-boundary reads safely degrade to null and are not the current terminator;
- eight bindless-window messages form four repeated pairs: an oversized union at PCs `0x86c`/`0x850`
  and an unusable unbased global window at PCs `0x140`/`0x13c`; they remain bounded null fallbacks;
- `0x604258d000` clears launch through the new sixteen-wave path, validating blocker 48 in game;
- `0x9039cff00` is the exact failing queue-1 dispatch: 41 groups, local size `64x1x1`, 289 decoded
  instructions, 71 input/73 structured blocks, three loops, four buffer resources, and two
  bindings. Its malformed SPIR-V came from helper-inserted ballot selections inside a loop header;
- exact offline replay now validates and executes that shader with 41 workgroups after the
  branch-free ballot fix;
- the reported freeze is at frame 1932; no new import, audio, or texture-cache terminator precedes
  the queue-1 device loss;
- shaders `0x605ac67400`, `0x605ac68a00`, and `0x9039ba900` hit the duplicate-merge CFG gap but
  successfully emitted 81,730-, 90,087-, and 6,725-word dispatcher modules. They are one separate
  high-risk CFG implementation family.

## Known-gap count and playability estimate

`context.cpp` has 14 `EXIT_NOT_IMPLEMENTED` call sites, but they are Vulkan-result and internal-
invariant guards rather than 14 missing game features. Line 540 only reports that an earlier queue
operation lost the device; implementing another branch in that file cannot repair the shader that
caused the loss.

The accumulated logs expose six implementation families, counted by generic behavior rather than
by repeated warning or shader instance:

1. Bounded multi-wave wave64 emulation for straight-line compute workgroups: blocker 48 is complete
   and validated in game.
2. Structurally valid workgroup-wave64 ballot lowering inside guest loops: blocker 49 is complete,
   exact-replay validated, and is the immediate in-game retest gate for `0x9039cff00`.
3. Duplicate-merge CFG structurization shared by three shaders: pipelines currently succeed through
   dispatcher fallback, but this remains correctness, size, and GPU-risk debt.
4. Bindless range provenance/materialization: two repeated null-fallback symptoms, treated as one
   implementation family.
5. Native paired-register FP64 FMA (`V_FMA_F64`): capability-gated and not currently required for
   the offline non-RT path because the affected dispatch is safely skipped.
6. BVH ray intersection (`IMAGE_BVH_INTERSECT_RAY`): one feature family used by two shaders and
   deferred with ray-tracing modes outside the initial target.

Therefore the known immediate blocker is implemented but unvalidated in game; the visible backlog
is six families, two of which are deliberately deferred capabilities. The plausible near-term
pre-playable core remains one to three additional families depending on whether blocker 49, the
successful CFG fallbacks, and bindless nulls cause visible or stability failures after frame 1932.
There is no
defensible exact count to full playability yet: execution still stops before player control and
cannot expose later streaming, presentation, input, save, or audio defects.

The Ghidra import campaign remains useful, but the current log confirms that the previously
implemented NpUtility, VideoRecordingP, and AGC handlers resolve. Graphics aliases, descriptor
contents, shader variants, PM4 streams, and queue lifetimes remain execution-driven work.

## Phase A - Structured loop-latch shader and align-byte operation

Status: compiler and ISA fixes complete; progression evidence retained.

- The blocker-39 dispatcher path did not recur: `0x9039cff00` now structurizes at 72 blocks without
  fallback. The hash later lost the device for a distinct shader-semantics problem tracked below.
- The VOP3 `0x14f` skip is gone, with zero read-only buffer-owned image null aliases.
- The scalar-provenance convergence failure in `0x903957400` was tracked as blocker 41 and cleared
  in game.

## Phase A2 - Retest scalar-provenance convergence

Status: complete in game.

- The same quiet run advanced from the 71-message freeze to 77 shader messages.
- `0x903957400` completed scalar provenance, SRT planning, resource tracking/materialization,
  binding allocation, and SPIR-V emission.
- The first new deterministic blocker is queue 0 device loss in `0x9039cff00`, tracked as blocker
  42.

## Phase A3 - Retest lane selection and GLC polling semantics

Status: complete in game as progression evidence; insufficient by itself.

- `0x9039cff00` remained a 72-block structured shader without dispatcher fallback.
- Queue submission 38748 and the 77-message checkpoint were passed; execution reached submission
  43708 and 79 shader messages.
- The same shader still lost the device, which promoted the two-dword atomic audit to blocker 43.

## Phase A4 - Retest indivisible 64-bit buffer exchange

Status: complete in game as a negative result; the native atomic did not clear the terminator.

- The same quiet run again reached 79 shader messages and lost queue 0 at submission 30689 after
  `DispatchDirect(6, 1, 1)`.
- The smaller 24,705-word live module confirms the native qword exchange path was compiled.
- Exact shader replay promoted the malformed dual-purpose loop header to blocker 44.

## Phase A5 - Retest split conditional loop header

Status: complete in game as a negative result; the valid CFG did not clear the terminator.

- The live shader reports structured CFG success at 73 blocks with no dispatcher fallback.
- Its 24,712-word module and compute pipeline create successfully.
- Queue 0 still loses the device at the 79-message checkpoint, promoting atomic ordering to blocker
  45.

## Phase A6 - Retest storage-buffer atomic publication ordering

Status: complete in game as a negative result; ordered atomic publication did not clear the
terminator.

- The live shader remains a 73-block structured program with 24,712 SPIR-V words and a successfully
  created compute pipeline.
- Queue 0 lost the device at submission 26839 after `DispatchDirect(41, 1, 1)`, again at 79 shader
  messages. Dispatch widths of 2, 3, 6, and 41 across runs confirm that no fixed group count is the
  progress gate.
- The exact kernel uses `BUFFER_ATOMIC_ADD` to allocate a ticket, publishes state/value pairs with
  two native qword exchanges, and polls earlier pairs with `GLC BUFFER_LOAD_DWORDX2` inside an outer
  lookback loop. The next audit must correlate those operations with the four bound buffer ranges.
- Do not restore split atomics, weaken the qword operation, or change cache ownership without new
  alias evidence.
- Continue collecting `CS skip`, bindless-window, image-boundary, unresolved-import,
  `DEVICE_LOST`, and negative-`VkResult` lines.

## Phase A7 - Audit qword polling and live descriptor ranges

Status: complete in game as progression-positive but insufficient.

- MUBUF dword-pair loads were lowered into two unrelated per-component IR instructions. When the
  original operation is untyped `BUFFER_LOAD_DWORDX2 GLC` and its exact tracked buffer resource is
  also published by `AtomicSwapU64`, component zero now emits one uint64 `OpAtomicLoad` and stores
  both result dwords; the covered component-one instruction emits nothing.
- The pair path requires eight-byte runtime alignment and a qword index within the uint64 descriptor
  view. Invalid pairs return zero through the existing robustness policy. The atomic load carries
  device-scope acquire `UniformMemory` semantics (`0x42`), directly synchronizing with the release
  side of the qword exchange.
- Ordinary dword loads, unmatched GLC pairs, typed/formatted loads, other resources, and non-GLC
  pairs retain their previous lowering. This is a resource-contract fix, not a title or shader-hash
  exception.
- Preserve the wave-size lane fix, native uint64 exchange, split loop header, and atomic acquire-release
  semantics while isolating this blocker.
- Binary SPIR-V coverage requires exactly one 64-bit `OpAtomicLoad` with semantics `0x42` and proves
  an ordinary GLC dword load is not strengthened. A Vulkan regression performs a native qword
  exchange, reacquires the published pair through `BUFFER_LOAD_DWORDX2 GLC`, and verifies both
  dwords. The in-game module changed from 24,712 to 24,228 words and reached 81 shader messages, so
  the path is live; device loss in the same kernel promoted wave-size decoding to blocker 47.

## Phase A8 - Correct dispatch wave size and emulate one guest wave64

Status: code and automated validation complete; launch shaders use the corrected wave64 path, while
the former `0x9039cff00` checkpoint still needs an in-game retest after blocker 48.

- Decode wave32 from `COMPUTE_DISPATCH_INITIATOR.CS_W32_EN` bit 15. Keep
  `COMPUTE_PGM_RSRC1` bit 30 as `MEM_ORDERED`; it is not a wave-size flag.
- The exact `64x1x1`, mode-`0x41` kernel is wave64. A bounded workgroup-backed path combines two
  host wave32 subgroups into one guest wave for ballot and lane exchange without aliasing guest LDS.
- The initial mode was limited to exactly one 64-thread wave per workgroup. Phase A9 extends it only
  to structurally uniform multi-wave programs with isolated scratch; arbitrary multi-wave control
  flow still cannot be modeled by unconditional workgroup barriers.
- Static checks require workgroup scratch, barriers, and atomics and forbid subgroup ballot/shuffle.
  The Vulkan test enables only guest lane 63 and verifies `V_READFIRSTLANE_B32` returns 63 to all 64
  invocations.

## Phase A9 - Isolate straight-line multi-wave wave64 workgroups

Status: complete in game.

- The blocker-48 launch shader uses `32x32x1` local invocations, or sixteen guest wave64 waves. Host
  subgroup size control cannot request 64 because the device reports `min=max=32`.
- Replace the Boolean workgroup-wave64 marker with an explicit wave count. Allocate 66 dwords per
  wave and index each lane/ballot slice from flattened local invocation index. The observed
  sixteen-wave case uses 1,056 dwords, separate from guest LDS.
- Admit multi-wave workgroups only when total invocations are a multiple of 64 and no greater than
  1,024, and structured IR contains only unconditional branches/returns with no loop header or
  dispatcher fallback. This proves every invocation reaches the stronger workgroup barriers in the
  same structural order. Continue rejecting conditional, indirect, looping, partial-wave, and
  oversized cases.
- Static coverage verifies 132-dword scratch and per-wave indexing for two waves and rejects a
  conditional variant. A Vulkan test makes wave 0 read lane 0 and wave 1 read lane 64, proving the
  scratch slices do not alias.
- The retest cleared the former five-shader launch fatal, reached frame 1932 and 82 shader-compile
  messages, and then failed in the distinct single-wave looped kernel tracked as blocker 49.

## Phase A10 - Keep wave64 ballot helpers out of guest control flow

Status: code, exact replay, and full automated validation complete; in-game validation pending.

- Queue 1 submission 44572 lost the device while executing `0x9039cff00` as
  `DispatchDirect(41, 1, 1)`. The pipeline created successfully, so `context.cpp:445` only surfaced
  the asynchronous failure.
- Exact ELF replay made SPIR-V validation deterministic. A back edge targeted the original guest
  loop-header label, while two conditional regions inserted by `EmitWaveBallot` caused
  `OpLoopMerge` to be emitted in a continuation label. This violated SPIR-V loop structure.
- Replace conditional scratch initialization and predicate publication with branch-free per-lane
  `OpAtomicAnd`/`OpAtomicOr` operations separated by workgroup barriers. Every invocation clears
  its unique bit and conditionally ORs either that bit or zero; no helper selection block is needed.
- Preserve per-wave 66-dword scratch isolation and the multi-wave structural safety gate. This is a
  generic workgroup-wave64 emission fix, not a shader-address exception.
- The exact 289-instruction shader now validates and completes a synthetic 41-workgroup Vulkan
  dispatch. Static coverage requires a loop merge plus atomic clear/set and rejects any selection
  merge; `WorkgroupWave64BallotInLoop` executes the generic loop shape through Vulkan.

## Phase B - Shader instruction coverage

### B1 - `MUBUF 0x3f` and `0x40`: complete

RDNA2 identifies opcode `0x3f` as `BUFFER_ATOMIC_FMIN`. The decoder, IR, resource tracking, and
SPIR-V emitter now lower it as an ordered F32 minimum implemented with an atomic compare-exchange
loop. GLC returns the successfully observed old value. A Vulkan-executed regression covers normal
minimum, source NaN, memory NaN, equal signed zero, SPIR-V validation, and returned-old behavior.
Opcode `0x40` is `BUFFER_ATOMIC_FMAX` and uses the corresponding ordered `src > old` CAS update.
Its Vulkan regression covers the same ISA edge cases and GLC return contract.

### B2 - `VOP3 0x14f`: complete

RDNA2 identifies opcode `0x14f` as `V_ALIGNBYTE_B32`. Decoder, IR, and SPIR-V lowering implement
the low 32 bits of `(src0:src1) >> ((src2 & 3) * 8)`. Static decoder/IR coverage and a Vulkan-
executed count-2 case pass.

### B3 - `VOP3 0x14c`: capability-gated implementation track

RDNA2 identifies opcode `0x14c` as `V_FMA_F64`, a fused double-precision multiply-add. Do not map
it to an F32 operation or the existing bit-manipulation-only F64 conversion helpers. A correct
implementation requires all of the following as one tested change:

1. Vulkan `shaderFloat64` support queried and enabled, with a deterministic unsupported-device
   policy.
2. SPIR-V Float64 capability and a 64-bit float type emitted only for shaders that need it.
3. Paired SGPR/VGPR source and destination operands in decoder and IR lowering, including legal
   register-pair validation and F64 source modifiers.
4. Fused `OpFusedMultiplyAdd` or an equivalently guaranteed fused lowering with correct destination
   pair storage.
5. Vulkan execution tests for register pairs, modifiers, NaN/Infinity, and fused-rounding cases.

This track is safe to implement independently, but should not block testing the current cache fix
unless the next run proves the skipped dispatch is progression-critical.

### B4 - `MIMG 0xe6`: ray-tracing scope, deferred

RDNA2 identifies opcode `0xe6` as `IMAGE_BVH_INTERSECT_RAY`. It is not an ordinary image load,
sample, or atomic. Correct support requires the guest BVH descriptor format, ray operands/results,
host ray-query or software traversal policy, resource tracking, synchronization, and conformance
tests. Keep the two current shaders skippable until that subsystem is deliberately scoped. Do not
route this opcode through the existing sampled/storage image emitter.

### B5 - SRT reader and dynamic descriptor failures

- The repeated `ReadConst pc=0x0c` failures had fixed offsets and dispatch-varying mapped bases;
  they were not dynamic-offset provenance failures. The runtime reader now admits mapped,
  GPU-owned guest pages so the memory tracker's CPU-fault path can synchronize them. Status:
  complete and still clear in the latest run.
- Revisit lane-mask-selected four-dword descriptors only if the current log reports that failure;
  earlier two-dword address flavors are already handled.
- The oversized bindless union must not be made bindable by removing size checks. First identify
  the bad contributor or split the logical address space into bounded resources.

### B6 - Scalar-provenance fixed-point convergence: complete

An allocated phi is now a monotonic fixed-point boundary. If later work-list visits temporarily
deduplicate its incoming values to one identity, the analyzer updates the phi arguments but retains
the phi result instead of dropping back to the lone value. This fixes `0x903957400` without forcing
phis for invariant values. Keep the sequential-loop convergence regression and the exact offline
shader replay as the code and title-specific gates, respectively.

### B7 - Wave-size lane selection and globally coherent polling: code complete

`V_READLANE_B32` and `V_WRITELANE_B32` selectors are modulo the guest wave size. The emitter now
uses mask 31 for wave32 and 63 for wave64, preventing an out-of-range host subgroup-shuffle index in
genuine wave32 shaders. The observed `0x9039cff00` selector 63 was initially used to motivate this
fix, but blocker 47 proves that exact kernel is wave64; the generic lane-mask repair remains correct.
For storage-buffer operations carrying the guest `GLC` bit, the shader-local storage block is
coherent and each load is volatile with a device-scope acquire-release `UniformMemory` barrier
before it. Ordinary buffer loads retain the existing path. Keep both generic SPIR-V regressions.

### B8 - `BUFFER_ATOMIC_SWAP_X2`: native 64-bit exchange, code complete

The x2 opcode is one indivisible 64-bit exchange, not two independently ordered dword exchanges.
It now lowers through a dedicated paired IR opcode to one uint64 `OpAtomicExchange`. Only shaders
that need it declare `Int64`/`Int64Atomics` and a qword storage-block view. The dword and qword
variables share the existing storage-buffer descriptor binding, are both marked `Aliased`, and use
four- and eight-byte array strides respectively. Alignment and runtime-array bounds are checked;
`GLC` decomposes the single returned qword into the destination register pair. Vulkan startup
requires and enables `shaderBufferInt64Atomics`. Static SPIR-V and Vulkan-executed regressions pass.

### B9 - Conditional loop-header structurization: code complete

A guest loop header can begin with a conditional that only skips optional work before an internal
join; the actual loop exit can occur later in the body. Such a block cannot directly own both the
SPIR-V loop and nested selection constructs. CFG structurization now inserts an empty loop header,
redirects all incoming and back edges through it, recomputes dominance and natural loops, and lets
the original conditional receive its own selection merge. Canonical loop headers with a direct edge
to the loop merge are unchanged. The exact `0x9039cff00` replay grows from 72 to 73 blocks and
validates, while a focused rotated-loop regression protects the generic shape.

### B10 - Storage-buffer atomic publication ordering: complete, but insufficient alone

Storage-buffer atomics now carry device-scope acquire-release `UniformMemory` semantics on the
atomic instruction itself. This applies to the generic 32-bit read/modify/write path and the native
64-bit exchange used by `BUFFER_ATOMIC_SWAP_X2`. A post-atomic barrier alone cannot release writes
which precede the atomic publication, so retaining only that old pattern was insufficient for the
observed cross-workgroup queue/scan protocol. Binary-level tests require the exact semantics operand
for both widths and validate the resulting SPIR-V; the Vulkan qword-exchange regression continues to
pass. The in-game blocker-45 retest still lost the device, promoting qword polling and descriptor-
range identity to phase A7 rather than weakening these semantics.

### B11 - Matched GLC qword polling acquire: code complete

An untyped two-dword GLC load whose tracked storage-buffer resource is published through
`AtomicSwapU64` now uses a single aligned, bounds-checked uint64 atomic acquire. This preserves the
guest pair as one observation and establishes a direct acquire/release relationship with the native
qword exchange. All other loads remain on their existing scalar path. Binary SPIR-V validation and
Vulkan execution cover the strengthened and non-strengthened cases.

### B12 - Branch-free workgroup-wave64 ballot: code complete

Workgroup-wave64 ballot scratch maintenance must not synthesize structured selections inside the
current guest block. Each invocation now selects its own low/high ballot word, atomically clears its
unique lane bit, synchronizes the workgroup, selects `bit` or zero from the predicate, and performs
an unconditional atomic OR before the result loads. Because all 64 bits are independently cleared,
this resets the ballot without a lane-zero conditional and remains valid for isolated multi-wave
scratch slices. The exact `0x9039cff00` replay and permanent loop-header regressions prove that the
emitted back edge targets the real `OpLoopMerge` header.

## Phase C - GPU and cache correctness

- Keep blockers 32, 35, 36, and 38 narrow. Blocker 35 requires a non-exact raw window fully
  containing a page-aligned storage image with no GPU-current bytes. Render-target partial writes
  require a page-aligned, non-GPU-current target and actual byte overlap; byte-disjoint page
  contact, unaligned partial targets, and mixed ownership remain unsupported.
- If a future run reports a completed cross-queue alias, require an advanced recording generation
  or a signaled submission fence. A live producer needs scheduler synchronization, not retirement.
- If a freeze recurs, use the new analysis-phase brackets to distinguish CPU compiler work, a guest
  spin, and GPU failure before adding recovery behavior.
- Blocker 41 was CPU-side scalar analysis; blockers 42 through 47 and blocker 49 concern one
  post-pipeline polling kernel. Blocker 48 is a distinct launch-time multi-wave subgroup-policy gap
  exposed by the same corrected wave decoding and is now validated in game. Reopen cache ownership
  only if a retest supplies new alias evidence.
- The blocker-43 atomicity audit is complete: retain the single native 64-bit exchange. Do not infer
  a return to blocker 39's dispatcher failure. Blocker 44 was a separate structured-CFG validity
  defect and is confirmed live at 73 blocks. Blocker 45 attaches release/acquire semantics directly
  to its storage-buffer atomics. Blocker 46 adds the matched qword acquire, blocker 47 corrects
  wave-size decode and cross-half wave64 communication, blocker 48 isolates straight-line
  multi-wave scratch, and blocker 49 keeps ballot helpers structurally branch-free; keep all six
  repairs in place during the next game validation.
- Treat the two successful but very large duplicate-merge dispatcher modules as GPU-risk debt. Fix
  their shared CFG shape before they become a device-loss terminator; do not raise fallback budgets.
- Treat null-binding at an incoherent image/metadata boundary as an intentional safety result;
  extend only from evidence that the shader accesses a coherent prefix which can be represented.

## Phase D - ABI verification

- Continue checking that NpUtility, VideoRecordingP, and AGC imports remain resolved.
- Add an offline handler only for a currently called, still-unregistered NID with a recovered ABI
  and bounded platform policy. Do not add title-ID-specific success stubs.

## Safe implementation and commit order

Keep the existing four-commit strategy; do not commit the dirty tree as one snapshot:

1. `loader: export SELF and import inventory for analysis`
2. `graphics: support dynamic compute resources and coherent aliases`
3. `libs: implement observed offline platform contracts`
4. `docs: update GTA V bring-up record`

The graphics commit must include the decoder/IR/emitter changes, descriptor materialization,
texture/buffer ownership and queue-lifetime rules, CMake wiring, and their regressions together.
Blockers 31-49, both F32 buffer atomics, the native 64-bit exchange, loop-control and conditional
loop-header splitting, scalar-provenance, wave-size lane, single/multi-wave workgroup isolation, and
GLC visibility fixes, plus `V_ALIGNBYTE_B32`, belong in that graphics contract. Preserve the newest
GTA stash until the split commits build from a clean index and a later in-game retest passes. Do
not stage `_DownloadData/`, `_TempData/`, `gtav-work/`, extracted game ELFs, import inventories,
logs, shader/pipeline dumps, or game paths.

## Upstream graphics-refactor integration

`upstream/main` advanced from the already merged `587cb23` to `bd9086e`. The four-commit range
removes Vulkan SDK coupling, migrates the backend to Vulkan-Hpp, centralizes transfer helpers, and
mechanically refactors most renderer files. It does not contain the corrected dispatch-initiator
wave-size decode or the workgroup-wave64 implementation, so it is not expected to fix blocker 47 by
itself. It may still improve long-term transfer, synchronization, and Vulkan maintenance, but its
95-file overlap makes an in-place merge too risky for the known GTA baseline.

The isolation plan is complete. `feature/gtav-compatibility` is preserved at `3e9e0b9` after the
four audited commits (`31467bb`, `1b4f1b4`, `7163ff6`, `3e9e0b9`). The active integration branch is
`feature/gtav-upstream-bd9086e`, created from that exact tip and merged with `upstream/main` at
`bd9086e`. No stash was applied and the three recovery stashes remain intact.

Twenty-one conflicts were resolved by keeping upstream's Vulkan-Hpp handles, centralized transfer
helpers, and resource-lifetime structure while porting the existing GTA cache, metadata,
descriptor, command-generation, and workgroup-wave64 semantics to those interfaces. A subsequent
compiler-driven audit restored format-usage texture selection, shader-address write propagation,
the texture address updater, DCC initialization/readback, and the affected regression coverage
where mechanical conflict resolution was insufficient. Release `kyty_emulator` builds, the focused
shader suites pass, all eleven standalone regression executables pass, and the diff checks are
clean. The refactor is therefore code-safe enough for comparison testing, but it is not yet the
new gameplay baseline.

The first comparison run exposed blocker 48 during launch: a straight-line `32x32x1` wave64 shader
needed sixteen isolated guest waves. The same single-wave guard exists on the fallback branch, so
this does not by itself disqualify the refactor. The generic bounded multi-wave fix now builds on the
integration branch, and its in-game retest clears launch. The later frame-1932 loss is blocker 49,
a generic invalid-SPIR-V defect in the shared workgroup-wave64 ballot helper rather than evidence of
a cache-refactor regression. Exact replay and all eleven tests pass with the branch-free helper.

Next action: rerun the same quiet Story Mode reproducer from
`feature/gtav-upstream-bd9086e`. Confirm the launch shader still selects sixteen-wave isolated
workgroup emulation, then verify that `0x9039cff00` passes frame 1932 and queue-1 submission 44572.
Record the next completed submit, shader address, visible state, audio state, and first fatal
diagnostic. Keep `feature/gtav-compatibility` as the immediate fallback until the refactored branch
reaches player control or establishes a clearly superior stable checkpoint.

## Verification gate

- Build Release `kyty_emulator` and every affected test target.
- Run `shader_recompiler_compute_tests` for host ownership tests, static decoder coverage, SPIR-V
  validation, and Vulkan execution.
- Before committing the graphics group, run all eleven standalone regression executables and
  `git diff --check`.
- Perform the user-run Story Mode retest after the code gate; update this file and
  `GTAV-bringup-plan.md` with the next observed blocker every iteration.
