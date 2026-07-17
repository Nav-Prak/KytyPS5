# Grand Theft Auto V PS5 bring-up plan

This document is the engineering plan and running bring-up record for a legally obtained
PlayStation 5 copy of *Grand Theft Auto V* in offline Story Mode. It records technical milestones,
but it is not a playability claim.

Plan date: 2026-07-17

Starting source branch: `feature/hades-compatibility`

Starting source commit: `d30dfac`

## Bring-up status

Tested title: `PPSA04264`, application version `01.005.000` (PS5 SDK 6.00).

Status on 2026-07-17:

- The loader enters guest main and the title completes substantial platform and asset
  initialization.
- The title renders its logos, language and calibration screens, front-end artwork, Story/Online
  tabs, and settings pages. Controller navigation works and observed frame rates range from roughly
  17 to 60 FPS. M3 is achieved; M4 remains partial until audio is verified.
- Some front-end glyphs are missing or fragmented even though surrounding text and artwork render
  correctly. This remains a separate graphics correctness issue.
- Selecting Story Mode reached a deliberate guest fatal assertion after `sceKernelBatchMap`
  returned `KERNEL_ERROR_ENOMEM` for a fixed direct-memory map. The range was an ordinary Windows
  reservation that overlapped stale placeholder bookkeeping, so `MEM_REPLACE_PLACEHOLDER` could not
  install the backing view. Fixed direct maps now consult the authoritative range metadata and
  recover with an ordinary fixed file-view map in that case. A visible retest passed the former
  assertion and reached 10% of Story Mode loading.
- The next blocker was a write-only `R8G8B8A8_UINT` storage image using `image_store_mip` with a
  descriptor that exposes only mip level zero. Kyty now collapses this safe single-level case to
  the bound Vulkan storage view, while retaining strict rejection for genuine multi-mip storage
  and read/write non-identity swizzles. The following resource used the same contract with a
  Standard4KB tile. Storage upload, binding, and coherent guest readback now support that one-level
  layout as well. A visible M5 retest is pending.
- `VideoRecordingP_v1` is still an unresolved called stub. It has not yet been shown to block the
  offline startup path.
- The host used for this run did not expose an SDL/WASAPI audio endpoint. Kyty continued with its
  timing fallback, so audible output remains untested.

Four deterministic compatibility blockers were fixed generically during the first session:

1. A host reservation occupied GTA V's fixed guest mapping range. Windows builds now reserve a
   bounded placeholder arena for fixed guest mappings and can transactionally replace overlapping
   placeholder-backed flexible mappings.
2. GTA V aliases render targets as storage images. The cache now retires fully contained,
   same-context render targets after synchronization and exact readback; partial or buffer-owned
   aliases remain rejected.
3. GTA V uses the PS5 single-sample depth preset `TILE_MODE_INDEX=1`, `ITERATE_FLUSH=1`, and
   `DECOMPRESS_ON_N_ZPLANES=5`. Kyty maps that exact control preset to its existing
   `SW_64KB_Z_X` depth representation while retaining strict rejection for other unsupported depth
   layouts.
4. A fixed `sceKernelBatchMap` replacement can target an ordinary Windows reservation that overlaps
   stale placeholder bookkeeping. Kyty now distinguishes the reservation from a real placeholder
   and installs the direct-memory file view with the correct fixed-map path. Focused regressions
   cover both partial direct-view replacement and recovery from stale placeholder tracking.
5. `image_store_mip` can be emitted for a storage descriptor that exposes only level zero. Kyty now
   accepts that single-level case, including the console's write-only integer BGRA selector,
   without pretending to support per-mip storage views.
6. One-level Standard4KB storage images now use the existing PS5 detiler on upload and a matching
   inverse tiler on GPU-to-guest readback. A deterministic `R8G8B8A8_UINT` round trip covers the exact
   layout class used during Story Mode loading.

Focused virtual-memory, image-alias, shared-tracker-page, and depth-preset regressions cover these
changes. Raw logs and local paths remain outside the repository.

## Initial target

The first useful target is deliberately narrow:

- Offline Story Mode only.
- A new local save and one local user.
- Non-ray-traced Performance Mode.
- One SDL-compatible controller with ordinary rumble; advanced DualSense features are optional.
- Boot through the legal screens and menus, start a new game, complete the opening playable section,
  enter the open world, and drive continuously for at least 30 minutes.
- Correctness and deterministic behavior before performance tuning.

Fidelity Mode, Performance RT, GTA Online, save migration, cloud services, trophies, Rockstar
Editor, advanced haptics, and perfect audio spatialization are not part of the initial target.

Rockstar documents three PS5 graphics modes. Performance Mode avoids ray tracing, while Fidelity
and Performance RT use it. Performance Mode is therefore the only planned mode until guest ray
queries and acceleration-structure behavior are implemented generically.

Reference:
<https://www.rockstargames.com/newswire/article/172872k8a375k8/gtav-and-gta-online-coming-march-15-for-playstation-5-and-xbox-series>

## Ground rules

1. Use only a legally obtained, locally dumped copy and required files from hardware the tester is
   authorized to use.
2. Never commit game files, keys, firmware, proprietary modules, save data, shader dumps, command
   buffers, RenderDoc captures, screenshots containing copyrighted artwork, or logs containing
   local game paths.
3. Keep raw diagnostic artifacts in a non-versioned work directory outside the `KytyPS5` Git
   worktree. Use placeholders in documentation.
4. Do not modify the game dump. Record its identity before the first run and treat it as read-only.
5. Retain only generic emulator fixes. Do not gate behavior on title ID, executable hash, shader
   hash, frame number, guest address, or a local path.
6. Do not bypass ownership, synchronization, descriptor, or memory guards merely to advance a
   frame. Replace a guard only after the guest contract is understood and regression-tested.
7. Work on offline Story Mode. Do not attempt to connect the emulator to GTA Online or emulate
   authentication against production services.

## Success ladder

Each milestone has an observable exit criterion. A later milestone cannot compensate for a failed
earlier one.

| ID | Milestone | Exit criterion |
| --- | --- | --- |
| M0 | Reproducible baseline | The exact dump, executable, source commit, host, driver, flags, and logs are recorded without committing private artifacts. |
| M1 | Loader entry | `eboot.bin` and required modules load, relocation completes, and guest main is entered. |
| M2 | Platform initialization | Required users, content, save, pad, audio, and video-out services initialize without called unresolved imports or fatal ABI errors. |
| M3 | First presentation | The emulator presents a changing, non-black frame with stable video-out ownership. |
| M4 | Front end | Legal screens and the main menu render, accept controller input, and produce intelligible audio. |
| M5 | Story start | A new offline Story Mode session begins in non-RT Performance Mode and reaches player control. |
| M6 | Opening sequence | The opening playable section completes without a deterministic crash, softlock, or essential rendering failure. |
| M7 | Open-world streaming | The player enters the open world and drives for 30 minutes while assets stream without corruption, runaway memory growth, or save damage. |
| M8 | Repeatable session | A saved session survives emulator restart, reloads, and repeats the tested route. |
| M9 | Playable performance | A warmed run has bounded stutter and acceptable frame pacing on the reference machine with diagnostics disabled. |

## Phase 0: freeze the Hades-derived baseline

Before making GTA V changes:

1. Confirm `feature/hades-compatibility` is clean and record its commit.
2. Rebuild `kyty_emulator` and all ten current regression executables.
3. Run the complete regression set and record the results.
4. Perform the pending Hades in-game retest on the merged branch if it has not already been done.
5. Create `feature/gtav-compatibility` from the verified commit.
6. Never mix local game artifacts into that branch.

The Hades working snapshot remains the behavioral comparison point. If a GTA fix regresses Hades,
the change is not ready unless the old behavior is proven incorrect and both paths receive tests.

## Phase 1: identify the test input without executing it

Record the following in a private baseline worksheet:

- Title and title ID.
- Content and application version.
- Dump method and date.
- Total file count and byte count.
- `eboot.bin` size and SHA-256.
- Module list and SHA-256 for each executable module.
- Available system/content metadata, PlayGo chunk count, and languages.
- Whether Story Mode is present locally rather than represented only by an Online entitlement.
- Host OS, CPU, GPU, Vulkan driver, free RAM, free VRAM, and SSD model/free space.

Do not put real local paths or hashes into the public plan. Add sanitized identity information to a
compatibility report only when it is safe and useful.

### Phase 1 decision gate

Proceed only if the dump is complete, readable, locally backed up, and accepted by Kyty's existing
SELF/ELF loader. A loader that cannot read the executable is a loader milestone, not authorization
to skip encryption or integrity checks.

## Phase 2: establish the quiet baseline

The first execution should minimize observer effects. In particular, do not enable RenderDoc,
Vulkan validation, shader dumps, command-buffer dumps, SPIR-V debug printing, graphics debug dumps,
or network profiling.

Use a new, non-versioned output directory for every run. From the `KytyPS5` directory, the baseline
shape is:

```powershell
$GtavGame = "<LEGALLY_OBTAINED_GTAV_DIRECTORY>"
$GtavRun = "<NON_VERSIONED_GTAV_WORK_DIRECTORY>\baseline-001"
New-Item -ItemType Directory -Force -Path $GtavRun | Out-Null

& .\_Build\windows\kyty_emulator.exe `
  --game $GtavGame `
  --file-read-min-latency-us 0 `
  --vulkan-validation false `
  --shader-validation false `
  --shader-optimization-type Performance `
  --shader-log-direction Silent `
  --command-buffer-dump false `
  --graphics-debug-dump false `
  --printf-direction File `
  --printf-output-file "$GtavRun\guest-printf.log" `
  --profiler-direction None `
  --spirv-debug-printf false `
  --pipeline-dump false `
  1> "$GtavRun\stdout.log" `
  2> "$GtavRun\stderr.log"
```

Start with the default zero file-read floor. The 100 microsecond Hades workaround is evidence about
Hades, not a safe GTA V default. Test timing changes only after reproducing a timing-sensitive
failure and keep zero as the control.

For the first run:

- Disconnect networking or prevent the title from entering Online flows.
- Allow up to five minutes if the process is making observable progress.
- Stop on the first deterministic fatal error, access violation, deadlock, or five minutes without
  new log/frame activity.
- Do not repeatedly continue past assertions in a debugger.
- Preserve the complete stdout and stderr files before changing flags or code.

### Baseline report

Summarize each run in a small text record:

```text
Run ID:
Source commit:
Game version:
Arguments:
Elapsed time:
Exit code:
Last visible frame:
Last guest printf:
Earliest deterministic blocker:
Peak host RAM / VRAM:
Artifacts captured:
```

## Phase 3: classify the earliest blocker

Classify before editing. Work only on the first deterministic blocker in this order:

1. Loader or relocation failure.
2. Called unresolved import.
3. Kernel ABI, memory-map, thread, event, semaphore, timer, or filesystem failure.
4. Platform service failure: user, content, save, pad, audio, video, dialog, trophy, or network-state
   query required by offline mode.
5. GPU command processor packet/register failure.
6. Shader decode, CFG, lowering, resource discovery, or SPIR-V validation failure.
7. Descriptor, image layout, tiling, metadata, aliasing, or ownership failure.
8. Host Vulkan error or device loss.
9. Guest deadlock or timing-sensitive softlock.
10. Visual, audio, input, or performance defect after forward progress continues.

Do not address later noise until the earliest blocker is understood. A black window accompanied by
an earlier called unresolved import is initially a platform problem, not a renderer problem.

## Diagnostic escalation ladder

Enable the least expensive diagnostic that can answer the current question.

| Blocker | First diagnostic | Escalate only if needed |
| --- | --- | --- |
| Loader/import | Existing loader and unresolved-import logs | Add focused module/NID logging with call-site and ABI data. |
| Guest crash | Native debugger stack, registers, and fault address | Add a narrow trace around the suspected ABI or memory transition. |
| Shader compile | `--shader-validation true` and one shader-log directory | Add focused IR/CFG logging for the failing shader; avoid dumping all shaders. |
| PM4/state | One bounded command-buffer dump | Add packet history and exact register provenance around the first rejected packet. |
| Vulkan validation | `--vulkan-validation true` for the shortest reproducer | Capture only the first validation error and its associated guest operation. |
| Visual corruption | Graphics debug dump around the first bad draw | Capture one RenderDoc frame only after presentation is stable and reproducible. |
| Deadlock | Debugger thread stacks plus queue/event state | Enable Tracy network profiling for a bounded run. |
| Performance | Warm, trace-free frame-time run | Tracy, allocation counters, and pipeline-creation timing in separate runs. |

Large dumps distort timing and can consume substantial disk space. Never use command-buffer,
shader, pipeline, graphics, RenderDoc, and profiler capture simultaneously unless a specific causal
question requires that combination.

## Phase 4: platform and ABI coverage

For every called unresolved import or incorrect service result:

1. Identify its module, library version, NID, callers, argument layout, return convention, and
   thread/callback behavior.
2. Determine whether the title requires real behavior, stable offline failure, or only a capability
   query.
3. Implement the generic PS5 contract; do not return unconditional success when state is expected.
4. Add a focused test for valid use, invalid parameters, lifecycle ordering, and concurrent use when
   relevant.
5. Confirm Hades and at least one other existing workload still pass.

Priority service areas are expected to be user/profile state, AppContent and PlayGo, save data,
controller, audio/AJM, video-out, dialogs, trophies/game intents, and an honest offline network
state. This list is a hypothesis until the import inventory and first call trace exist.

## Phase 5: graphics bring-up

Graphics work follows the actual first failing draw or dispatch. The main investigation tracks are:

- Unsupported PM4 packets and indirect command forms.
- GE/NGG stage configuration and any geometry or tessellation workload actually observed.
- RDNA 2 instruction decoding and exact EXEC/VCC/lane semantics.
- Scalar provenance, SRT traversal, descriptor discovery, and runtime resource specialization.
- Tiled, mipmapped, array, cube, volume, depth/stencil, and compressed formats.
- DCC/HTILE metadata ownership and partial clears or resolves.
- Buffer/image/render-target aliasing during streaming and compute post-processing.
- Indirect draws, compute queues, barriers, release/acquire ordering, and GPU-to-CPU readback.
- HDR and output conversion only after SDR/non-RT presentation is correct.

Ray-tracing modes remain out of scope. The existing Vulkan extension loader is scaffolding, not a
guest ray-query implementation.

Every graphics fix must include one of:

- A small synthetic command/state regression.
- A shader decode/lowering/SPIR-V regression using generic instruction words.
- A resource-tracking or layout regression with synthetic addresses and formats.

Do not add game-derived shaders, command buffers, addresses, hashes, or captures to tests.

## Phase 6: open-world streaming and memory

Once the game reaches player control, correctness testing changes from a single-frame problem to a
state-transition problem.

Use a repeatable route that exercises:

- Rapid driving across several districts.
- Camera rotation and abrupt direction changes.
- Entering and leaving interiors.
- Day/night or weather transitions available during the tested path.
- Vehicle changes, traffic, pedestrians, explosions, and pause/menu transitions.
- Save, quit, restart, and reload.

Record at one-minute intervals:

- Host committed memory and working set.
- Vulkan allocation bytes by heap and estimated VRAM use.
- Cached buffer, texture, render-target, framebuffer, descriptor, shader, and pipeline counts.
- Shader and pipeline compilations.
- File reads, bytes, latency distribution, and outstanding asynchronous operations.
- GPU queue submissions, waits, readbacks, and page-fault invalidations.
- Frame-time percentiles and the longest stalls.

Stop and investigate if memory grows monotonically over repeated loops, resource counts never
stabilize, old districts remain permanently resident, or page-fault/readback activity becomes
unbounded.

## Phase 7: audio, input, and persistence

### Audio

Validate basic stereo first: dialogue, effects, radio/music, pause/resume, seeks, channel changes,
and long-running decode. Missing Tempest 3D positioning is not an initial blocker. Codec errors,
buffer starvation, drift, or deadlocks are blockers.

### Input

Use a physical SDL-compatible controller. Validate sticks, analog triggers, buttons, deadzones,
rumble, disconnect/reconnect, focus loss, and simultaneous steering/trigger input. Keyboard input is
useful for menu recovery but is not an adequate GTA V gameplay test.

### Save data

Use a new emulator-managed save. Validate creation, autosave, manual save if exposed, clean close,
restart, reload, timestamps, and failure behavior. Back up the emulator save directory before any
save-format change. Imported console saves and cloud migration remain out of scope.

## Phase 8: performance architecture

Performance work begins only after a repeatable route is correct.

Highest-priority architectural work:

1. Persistent cache for recompiled shader artifacts keyed by guest code, stage, compiler options,
   ABI/resource layout, and emulator cache version.
2. Vulkan pipeline-cache persistence with driver/device identity and safe invalidation.
3. Background or scheduled compilation only where it preserves guest ordering and Vulkan safety.
4. Bounded texture/buffer/framebuffer/descriptor/pipeline caches with measurable eviction.
5. Reduced redundant transitions, uploads, readbacks, and page-protection churn.
6. Separate cold, warm, and diagnostic benchmarks.

Never compare performance between runs with different validation, dump, log, profiler, window,
driver, save, location, weather, or traffic conditions.

## Regression gates for each retained change

Before committing a fix:

1. Reproduce the old failure from an unmodified baseline or focused regression.
2. Add a generic automated test when practical.
3. Run the directly affected test executable.
4. Run all current regression executables before a milestone commit.
5. Rebuild `kyty_emulator` in Release.
6. Retest the shortest GTA V reproducer with diagnostics disabled.
7. Retest Hades at graphics, audio, input, and presentation milestones.
8. Inspect the diff for title IDs, hashes, guest addresses, frame counters, and local paths.
9. Record what was observed, what was inferred, and what remains untested.

Commit one causal fix at a time. Use milestone tags or immutable archive branches for the first
working menu, first gameplay, and first stable open-world session.

## Expected high-risk areas

These are planning risks, not claims about the untested executable:

| Risk | Why it matters | Early mitigation |
| --- | --- | --- |
| Shader-stage breadth | Kyty currently recompiles vertex, pixel, and compute stages and skips some unsupported GE configurations. | Inventory observed stage masks before implementing a new stage. |
| Pipeline explosion | Hades already experiences compilation stalls and the current pipeline cache is process-local. | Add compile counters immediately; design persistent caches before performance tuning. |
| Resource residency | A streaming open world can expose missing eviction and partial-mip/alias transitions. | Add per-cache counts and memory budgets before the 30-minute route. |
| I/O semantics | GTA V is designed around PS5 SSD streaming; a fixed latency workaround may hide races or cause stalls. | Preserve zero-latency control runs and measure real read distributions. |
| Multi-queue synchronization | Graphics, compute, copy, metadata, and CPU access can overlap. | Log queue ownership and release/acquire provenance around the first mismatch. |
| Offline platform behavior | Online-capable titles still make network, NP, trophy, and intent queries in Story Mode. | Return accurate offline state and implement required local behavior; do not fake service login. |
| Long-session drift | Short Hades tests do not prove stable resource counts, audio clocks, saves, or thread lifecycle. | Use repeatable 5-, 15-, and 30-minute soak gates. |

## First-session checklist

When the copy is ready:

1. Confirm it is stored outside the repository and mark the source directory read-only if practical.
2. Create a separate non-versioned GTA V work directory with ample free space.
3. Record identity and host information from Phase 1.
4. Verify the Hades-derived baseline tests and create the GTA branch.
5. Run the quiet baseline exactly once.
6. Preserve stdout, stderr, guest printf, exit code, and the last visible frame description.
7. Name the earliest deterministic blocker; do not begin several speculative fixes.
8. Select one diagnostic from the escalation ladder and reproduce the blocker.
9. Write a minimal synthetic regression before or alongside the generic fix.
10. Advance only to the next success-ladder milestone after the current one is repeatable.

## Definition of initial success

The initial project is successful when a legally obtained PS5 copy can start a new offline Story
Mode game in non-RT Performance Mode, complete the opening playable sequence, enter the open world,
drive for 30 minutes with correct essential graphics/audio/input, save, restart, and reload without
a deterministic crash, corruption, runaway memory growth, or title-specific emulator bypass.

Good average FPS alone is not success. A menu alone is not success. A single captured frame is not
success. The result must be repeatable from a documented source commit and clean launch procedure.
