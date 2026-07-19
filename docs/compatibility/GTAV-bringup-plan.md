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
  layout as well. A visible retest cleared these storage-image blockers and reached 40% of Story
  Mode loading.
- GTA V then selected array layer 3 from a four-layer Standard64KB color target. Kyty now preserves
  the full array backing while creating a single-layer attachment view and applies the existing
  Standard64KB upload/readback tiler independently to each slice. Exact pitch, per-slice size,
  total size, metadata, sample-count, and mip-count guards remain enforced. A visible retest
  cleared the blocker, entered the prologue, and rendered the "Ludendorff, nine years ago" title.
- The prologue then submitted the two-dword `IT_REWIND` packet emitted by `GraphicsDcbRewind`.
  Kyty now validates its length and sole `INITIAL_STATE` payload bit and consumes it as a marker;
  the bounded host command-buffer parser has no persistent hardware ring read pointer to rewind. A
  visible retest cleared the crash and reached the in-game title frame.
- The title frame then remained static while audio advanced and the guest repeatedly called two
  unresolved `Agc_v1` imports. Unresolved-import diagnostics now preserve the guest ABI while
  reporting the caller, module-relative offset, and first four integer arguments. Call-site
  analysis identified `b-oySn+G2tE` as `GraphicsAcbJumpGetSize`: GTA's ring buffer reserves its
  four-dword `IT_INDIRECT_BUFFER` packet at the end of each ACB segment and patches that slot to
  chain the next segment. Returning zero let command data overlap the reserved jump slot. Kyty now
  exposes the generic 16-byte packet size; a visible retest is pending. The other import is used as
  a cleanup/destructor-style call whose return value is ignored and remains unresolved.
- The next deterministic failure was GPU-to-guest writeback of a 128-bit-per-element RenderTarget
  surface. Kyty already detiled that format with the PS5 Standard64KB-128 equation; writeback now
  uses the exact reciprocal tiler, including strict 64x64-block pitch and allocation guards. Fixed
  equation anchors and a cross-block round trip pass on the host; a visible GTA V retest is pending.
- The called `VideoRecordingP_v1` startup NID is no longer unresolved. Bounded Ghidra analysis
  recovered its command/data ABI, the matching 4/8-byte query ABI, and a no-argument capability
  probe whose `0x80a80003` result is explicitly handled by GTA as recorder-unavailable. Kyty now
  accepts only the observed UTF-16 metadata commands, zeros bounded query outputs, and reports the
  recorder as unsupported before GTA allocates capture buffers. Six other recording NIDs remain
  unresolved because their caller contracts are not yet complete.
- The five `NpUtility_v1` NIDs reached during Story loading are also registered from a recovered
  worker state machine: two 0x38-byte request starts, a four-byte poll, an abort, and a 0x18-byte
  result. Both starts return Kyty's existing NP signed-out error, so the guest follows its explicit
  offline failure path without fabricated request IDs or network measurements.
- The host used for this run did not expose an SDL/WASAPI audio endpoint. Kyty continued with its
  timing fallback, so audible output remains untested.

Ten deterministic compatibility blockers have been fixed generically during bring-up:

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
7. Standard64KB color-target arrays now reuse the established Vulkan array-image, attachment-view,
   upload, and readback paths. A four-layer round trip and the observed layer-3 view contract guard
   against slice crossing and accidental footprint relaxation.
8. The command processor now recognizes the exact `IT_REWIND` packet generated by Kyty's AGC API.
   It accepts both defined initial-state values while rejecting alternate lengths and reserved bits.
9. 128-bit RenderTarget surfaces now use the same Standard64KB-128 equation in both directions.
   GPU-to-guest writeback preserves the existing format-specific layout instead of failing after
   the prologue title, and fixed byte-map anchors prevent a self-consistent but incorrect round trip.
10. ACB ring buffers now receive the correct 16-byte jump-packet reservation through
    `GraphicsAcbJumpGetSize`. Segment command data can no longer overlap the four-dword
    `IT_INDIRECT_BUFFER` slot later patched to chain the next segment.

Focused virtual-memory, image-alias, shared-tracker-page, and depth-preset regressions cover these
changes. Raw logs and local paths remain outside the repository.

Status on 2026-07-18 (prologue compute bring-up):

- The prologue's compute pipeline is RAGE's GPU-driven object system: descriptor tables indexed
  by data-driven `v_readfirstlane` waterfalls, V#s assembled in registers from 64-bit pointers,
  pointer-chasing global and scalar loads, and a 64-bit `buffer_atomic_swap_x2` allocator
  hand-off. None of this is resolvable by CPU-side descriptor walking; a family of runtime
  ("bindless") paths was added to the recompiler. Sessions alternated one observed blocker with
  one generic fix; each retest reached further into the prologue (60 pipeline compiles at last
  run, previously 56).
- A `--cs-skip-unresolved` option now converts compute recompilation failures into skipped
  dispatches with capped diagnostics instead of process aborts, turning each run into a full
  blocker catalog. Failures in static phases (decode, CFG, tracking, emission) are cached per
  program; runtime-snapshot failures retry every bind.
- Observed behavior progression across retests: crash at shader tracking → crash at storage
  binding alignment → freeze at the mission title card with audio running (64 written buffers
  null-bound) → freeze with audio also stopping (suspected GPU-side spin on memory pinned by a
  degraded null window) → a cache-ownership assertion after the tracked-buffer fallback advanced
  through 57 shader compiles. The new assertion was a clean render target wholly covered by a
  large writable raw-buffer window, rather than another compute-recompiler failure. The next
  retest reached the same ownership seam after 56 shader compiles with a different render target
  that was GPU-current, proving that containing raw windows require both stale-image invalidation
  and current-image readback paths. The first complete post-audit run cleared the restored
  Standard4KB, layered-target, and image-alias paths, compiled 55 shaders, displayed the
  `Ludendorff, nine years ago` card, and then reached the clean-storage counterpart described in
  blocker 28. Its retest cleared that transition, advanced visibly beyond the title card, and
  compiled 66 shaders: eleven more than the prior run and the farthest visible progress reported
  to date. It then reached the read-only V#/image boundary described in blocker 29. The next run
  cleared that boundary and reached the completed graphics-to-compute buffer lifetime transition
  described in blocker 30. Its 57-shader count reflects a different cache/pipeline schedule and is
  not evidence that the former 66-shader path regressed.
- The next two runs cleared the completed cross-queue lifetime and the clean partial-target alias.
  The latest run compiled 65 shaders with no unresolved imports or device-loss marker before the
  same broad raw window was rebound over a render target already owned by the buffer path. That
  repeated invalidation is the idempotent ownership transition in blocker 32. Shader count varies
  with the cache schedule and is not a monotonic progress measure.
- The blocker-32/33/34 retest cleared the repeated render-target invalidation, implemented
  `BUFFER_ATOMIC_FMIN`, and mapped-page SRT reads: the log contains no compute skips, unresolved
  imports, device-loss marker, or negative Vulkan result. The latest run compiled 59 shaders
  before reaching the repeated Standard4KB storage-image transition in blocker 35. Two oversized
  bindless windows still degrade to null, but neither is the terminating failure.
- The blocker-35 retest cleared that storage transition and reached the repeated partial-target
  ownership state in blocker 36. Its cache schedule exposed four unique compute skips: the newly
  reached `BUFFER_ATOMIC_FMAX` is implemented in blocker 37, while the capability-gated FP64 FMA
  and two BVH shader variants remain deferred. Imports and Vulkan/device status remain clean.
- The blocker-36/37 retest compiled 59 shaders with no compute skips or read-only null aliases,
  confirming both changes cleared. It then reached blocker 38: a 24-byte write wholly inside a
  clean render target, below the former arbitrary one-page invalidation threshold.
- The blocker-38 retest compiled 76 shaders, the furthest run so far, and did not repeat the
  24-byte alias assertion. It then reached blocker 39: `vkWaitForFences` returned
  `VK_ERROR_DEVICE_LOST` after compute shader `0x9039cff00` fell back to a 25,698-word dispatcher
  module. The same log exposed `V_ALIGNBYTE_B32`, now implemented as blocker 40; native FP64 FMA
  and the two BVH shaders remain capability-gated work.
- The blocker-39/40 retest cleared the device loss and `V_ALIGNBYTE_B32` skip, then froze visually
  after 71 shader-compile messages while audio and controller processing continued. Vulkan waits
  completed and the log contains no fatal error, negative `VkResult`, or unresolved import. The
  final compute shader `0x903957400` completed IR lowering but never began SPIR-V emission. Exact
  offline replay identified a non-converging scalar-provenance phi at the second of three sequential
  loops. Phi identities are now monotonic once allocated, the exact shader completes every static
  analysis stage, and blocker 41 is code-complete pending an in-game retest.
- `upstream/main` through `587cb23` is merged into the compatibility branch as `b817839`. The
  integration resolved nine conflicts while retaining upstream's split image/image-view cache and
  sparse multi-owner page index alongside the GTA storage, presentation, shader, and invalidation
  contracts. A staged pre-merge audit found and restored the integration omissions described in
  blockers 22 through 27 and the video-out/DCC audit below. The final pass compared all three local
  stash snapshots rather than only the merge-conflict files. The post-audit Release emulator builds
  and all eleven regression executables pass.

Deterministic compatibility blockers fixed in this phase (continuing the list):

11. Compute shaders fetch storage V#s at GPU time from descriptor tables (structured 80-byte
    records; record dword 1 selects a 16-byte V# in the same allocation). Resource tracking now
    classifies the pattern, resolves referenced entries at materialization, and binds the union
    of their validated ranges as one window; the emitted code reads the live SGPR quad, rebases
    onto the window, and drops null or out-of-extent descriptors.
12. Storage-buffer descriptor bases are only dword-aligned while Vulkan requires
    `minStorageBufferOffsetAlignment`. Every Buffers-kind binding now starts at the base aligned
    down to 256 and the low-bits remainder travels per-bind through the flattened SRT
    (layout: SRT reads, one remainder dword per buffer, bindless window bases) and is re-added
    to every access. Null-binding written misaligned buffers had frozen Story Mode loading.
13. `VirtualQuery`-protection-based readability is the wrong validity test for guest memory:
    the memory tracker sets GPU-owned pages to no-access and CPU touches resolve through the
    page-fault handler. Guest-validity checks now use committed-ness (`HostMemoryRangeIsMapped`).
14. The first `buffer_atomic_swap_x2` (MUBUF 0x50) implementation lowered both dwords and cleared
    the skipped dispatch, but approximated the operation as two exchanges. Blocker 43 later
    corrected that incomplete contract to one indivisible native 64-bit exchange.
15. Global/scratch memory operations and `s_load_dword` through runtime-computed pointers
    (address provenance unknown) now demote to an unbased path: the emitted code computes the
    full address from live registers (both FLAT forms: 64-bit vector pair, and scalar base pair
    plus 32-bit offset), rebases onto a window bound over the union of the dispatch's tracked
    buffers, and drops out-of-window accesses. Written unbased windows are permitted; demoted
    scalar loads keep their base-register pair and their user-SGPR roots in the push constants.
16. V#s assembled in registers from runtime pointers (descriptor dword 1 unknown) now track as
    "tableless" bindless buffers using the same live-register emission with the tracked-buffer
    union as their window. This also generalizes control-flow-selected and loop-varying raw
    descriptors, which previously rejected as unsupported GPU selection.
17. Descriptor-table scans that cannot produce a usable window (uninitialized or relocated
    tables) now fall back to the tracked-buffer union instead of a null binding, so GPU-side
    spin loops polling such memory observe real values instead of constant zeros.
18. A broad writable raw-buffer window may wholly contain a clean cached render target. This is a
    valid ownership transition: the image becomes stale and its next attachment bind publishes
    current buffer bytes to guest backing before reloading it. Buffer-to-image coherence now also
    resolves all tracker pages touched by a sub-page-aligned image, preventing a cleared page from
    retaining unmatched dirty-byte ownership. The clean containing-window cases pass in
    `shader_recompiler_compute_tests`; the full compute suite, the other nine regression
    executables, and a Release `kyty_emulator` build pass.
19. A containing raw-buffer window can also cover a GPU-current render target. Before transferring
    ownership, Kyty now waits for the target, reconstructs its exact tiled or linear guest backing,
    clears image GPU ownership, and publishes only that image range for the subsequent buffer
    upload. The surrounding raw-buffer bytes remain guest-current. Exact unformatted views,
    unaligned exact views, and contradictory image/buffer ownership remain rejected. Focused and
    full compute suites, all other regression executables, and a Release emulator build pass; a
    visible title retest is pending.
20. The next run showed that one broad writable window can contain several cached render targets.
    GPU invalidation is now a two-phase batch: it first classifies every byte-overlapping cached
    image and rejects the whole operation if any pair has incompatible ownership or shares a
    tracker page, then invalidates clean targets or synchronizes GPU-current targets over each exact
    image range. This prevents a late conflict from leaving half of the alias set transitioned.
    Focused regressions cover multiple clean/current targets, page-isolated batches, shared-page
    rejection, and exact-range transitions. The full compute suite, the other ten regression
    executables, and a Release emulator build pass; a visible title retest is pending.
21. The following run compiled 54 shaders before the same broad writable window wholly contained a
    clean, page-aligned sampled texture. This is the sampled-image counterpart of blocker 18: the
    texture may become buffer-current and reload from guest backing on its next use. Invalidation
    now classifies the complete overlap set before changing ownership, permits page-isolated clean
    sampled textures and render targets in one batch, and still rejects GPU-current or already
    buffer-current sampled textures, partial unaligned aliases, and any pair sharing a tracker
    page. Focused cases cover both containment directions, mixed page-isolated batches, and
    shared-page rejection. The Release emulator and all eleven regression executables pass; a
    visible title retest is pending.
22. The first post-merge run then regressed during initial render-target setup after seven shader
    compiles. Stack-map resolution identified `FindRenderTarget`: conflict resolution had retained
    the classifier that permits a same-context GPU-current target to retire for a byte-disjoint
    tracker-page neighbor, but omitted the existing pre-retirement readback list. The path once
    again waits for the device, reconstructs each retiring color target into guest backing,
    publishes the exact download range, and clears image GPU ownership before `RetireImages`.
    This is an upstream-integration correction rather than a newly discovered title contract. The
    Release emulator and all eleven regression executables pass; a visible early-boot retest is
    pending.
23. The blocker-22 retest passed the former seven-shader failure and compiled 15 shaders before a
    storage image created as `VK_FORMAT_R8_UINT` was sampled through an `R8_UNORM` view with the
    expected `R,R,R,1` swizzle. The pre-merge cache deliberately allowed the compatible
    `R8_UINT`/`R8_UNORM` pair and created the backing with mutable-format views, but both generic
    helper calls had been narrowed while the upstream cache was split into `image.cpp` and
    `imageView.cpp`. The split implementation now uses `IsMutableStorageSampledViewFormat` when
    creating storage images and `IsCompatibleStorageSampledViewFormat` when obtaining their sampled
    views. A focused Vulkan regression creates the mutable `R8_UINT` image plus both integer-storage
    and normalized-sampled views. The focused storage-view test, full compute suite, all ten other
    regression executables, and Release emulator build pass; a visible retest is pending.
24. The blocker-23 retest reached the menus and approximately 10% of Story Mode loading, compiling
    53 shaders before `FindStorageTexture` rejected a 64x64, one-level `R8G8B8A8_UINT` storage image
    in Standard4KB tile mode. Descriptor validation already admitted this exact layout and the
    existing detiler had reconstructed its 16 KiB guest footprint; the pre-merge Standard4KB
    acceptance branch and command/context null guard had been omitted from the merged cache path.
    Descriptor validation and cache creation now use one shared storage-tile policy accepting only
    linear, render-target, the format-gated Standard4KB set, and separately validated depth-tile
    layouts. The exact 64x64/16-KiB descriptor is covered by the storage regression. The focused
    Vulkan storage test, full compute suite, all ten other regression executables, and Release
    emulator build pass; a visible retest beyond 53 shader compiles is pending.
25. The blocker-24 retest passed storage-image creation but failed at the same Story Mode position
    when the GPU-current image was synchronized back to guest memory. The merged
    `DownloadColorImage` path had independently lost both Standard4KB readback admission and the
    `ImageInfo`-based inverse-tiler branch from the pre-merge code. A shared readback classifier now
    recognizes only single-layer, base-level-zero, one-level linear, render-target, and
    format-gated Standard4KB storage layouts. Standard4KB downloads use the existing exact inverse
    texture tiler; render-target-style storage continues to use the render-target equation. The
    regression now exercises the cache's classification for GTA V's exact 64x64, 16-KiB
    `R8G8B8A8_UINT` image, rejects multi-level and layered relaxations, and verifies the complete
    linear-to-tiled-to-linear round trip. The Release emulator and all eleven regression
    executables pass; a visible retest beyond the readback at 53 shader compiles is pending.
26. The blocker-25 retest cleared Standard4KB storage readback, reached 54 shader compiles, and
    invoked four previously unobserved `NpUtility_v1` imports before returning to the earlier
    four-layer Standard64KB color target. The log selected array slice `3..3` and supplied a
    16-MiB allocation whose computed per-slice footprint is 4 MiB. The pre-merge `FindRenderTarget`
    validator multiplied that slice footprint by `info.layers`; the merged validator compared one
    slice directly with the complete backing even though the shared classifier, image creation,
    attachment view, upload, and readback paths all retained their layered behavior. The exact
    layered-footprint predicate is shared again by classification and the production validator,
    with overflow, 64-KiB alignment, pitch, slice-size, and total-size guards intact. Focused
    coverage checks the same four-layer allocation and rejects a truncated three-slice backing.
    The focused Standard64KB test, Release emulator, full compute suite, and all ten other
    regression executables pass; a visible retest beyond 54 shader compiles is pending. The newly
    observed NP calls return through unresolved stubs but are not the cause of this graphics crash.
27. The complete stash audit found one remaining live feature whose only runtime consumer had been
    commented out by upstream commit `b434e3b`. Resource tracking still linked a four-dword buffer
    source to the matching eight-dword image sharp through `BufferResource::image_alias`, and
    materialization still validated the link, but descriptor binding ignored it. The runtime now
    null-binds a buffer only when its tracked alias is an active image sharp. It also restores GTA's
    exact write-only formatted-buffer use of a `R8G8B8A8_UINT`, 2D, RenderTarget-tiled,
    identity-swizzled image sharp as a buffer over the complete image footprint. The restoration is
    narrower than the old branch: unrelated nonzero buffer TYPE values and every nonmatching image
    shape retain the upstream buffer path instead of entering or fatally rejecting through the
    image path. Focused coverage checks the active tracked alias, exact texture-backed write, read/
    write rejection, alternate tile rejection, and preservation of a normal nonzero buffer TYPE.
    The focused storage descriptor test, Release emulator build, full compute suite, and all ten
    other regression executables pass; a visible GTA retest is pending.
28. The post-audit retest reached the prologue title card and compiled 55 shaders before a writable
    raw buffer window at `0x6003673600+0x3c30ca00` covered the clean, page-aligned 16-KiB
    Standard4KB storage image at `0x6022624000+0x4000`. This is a newly observed storage-image
    counterpart of blockers 18 and 21, not a pre-merge omission: the same state was unsupported in
    the audited stash. Because a clean image's guest backing is current, the buffer cache can seed
    its enclosing window before GPU writes and the existing storage rebind path can refresh the
    exact image range afterward. GPU invalidation now marks only a non-exact, fully containing raw
    alias over a clean, non-buffer-owned, page-aligned storage image as buffer-current. Exact raw,
    partial, GPU-current, already buffer-owned, and shared-tracker-page cases remain rejected.
    Focused cases reproduce the exact GTA addresses and sizes and cover mixed page-isolated storage/
    render-target batches. The focused ownership suite, full compute suite, all ten other regression
    executables, and Release emulator build pass. The subsequent run cleared this transition and
    reached 66 shader compiles; see blocker 29.
29. The blocker-28 retest cleared the title-card storage transition and compiled 66 shaders before
    `NativeStorageBuffer` tried to expose a read-only, unformatted V# at
    `0x60a5beeb00+0x2ffa790`. Stack-map and disassembly resolution identify the storage-buffer loop
    in `BindDescriptors`; this was not the nearby DMA packet, AGC import traffic, or the compute
    dispatcher's successful irreducible-CFG fallback. The V# begins on a cached image tracker page,
    just as neighboring runtime address resources at `0x60a5bee900` already report and null-bind.
    A native storage-buffer descriptor cannot represent holes, and exposing the whole range would
    let a raw buffer read stale image backing. Read-only V# binding now asks `BufferCache` for the
    largest coherent prefix only when GPU-current image or metadata ownership would otherwise make
    the alias fatal. A zero prefix null-binds the descriptor; a nonzero prefix binds only leading
    coherent bytes so normal descriptor bounds reject later accesses. Clean sampled-image aliases,
    byte-disjoint same-page reads, and every writable ownership-transition path remain unchanged.
    Coverage reproduces the GTA range, clean/GPU-current/page-only image distinctions, and metadata
    boundaries. The focused ownership suite, full compute suite, all ten other regression
    executables, and Release emulator build pass; a visible retest beyond 66 shaders is pending.
30. The blocker-29 retest no longer reached the read-only V#/image failure. It compiled a further
    compute pipeline and then expanded the cached buffer at `0x603d414000+0x38000` on logical
    compute queue 0 after that buffer had previously been used on logical graphics queue 8. The
    cache's queue mask was historical: it never distinguished a live producer from a command
    buffer whose submission fence had already signaled, so even completed cross-queue lifetimes
    were rejected. Cached buffers now record the exact command-buffer recording generation for
    each logical queue. Submission generations are published atomically, and a foreign queue bit
    can retire only when its generation has advanced after fence completion or its submitted
    generation's Vulkan fence is signaled. The existing same-family copy-and-barrier merge then
    preserves the completed buffer contents on the requesting queue. An unsubmitted or in-flight
    foreign generation remains a hard failure; the change does not pretend Kyty has a general
    cross-queue scheduler. Focused completion-state coverage rejects invalid, unsubmitted, and
    unsignaled generations and accepts only advanced or signaled submissions. The Release emulator
    and full compute suite pass; a visible GTA V retest is pending.
31. The blocker-30 retest cleared the cross-queue lifetime and compiled 68 shaders — the farthest
    visible progress to date — before a writable raw-buffer window at `0x60a5be5a00+0x2ffddf0`
    only *partially* overlapped a clean render target at `0x60a5be0000+0x870000`: the image begins
    before the window and its suffix is covered, so neither range contains the other. Blockers
    18–21 admitted clean-image invalidation only for the containment geometry, so this partial
    crossing hit the unsupported-alias assertion. A clean image (neither GPU- nor prior-buffer-
    modified) has authoritative guest backing, so any substantial raw-window overlap can mark it
    stale and reload from guest memory afterward regardless of overlap geometry: reloading the
    whole image reconstructs the correct contents whether or not the compute wrote a given byte.
    `ClassifyBufferImageWrite` now invalidates a clean, page-aligned render target on a page-or-
    larger byte overlap, while a sub-page edge nick — incidental adjacency between a small buffer
    and an image — stays on the strict path. GPU-current targets keep the readback-first
    synchronize path, and the exact-raw and containment contracts for storage textures, sampled
    textures, video-out, and depth targets are unchanged. Focused cases cover the GTA partial-
    crossing geometry and the edge-nick rejection. The full compute suite and the resource,
    stage-runtime, and scalar-provenance suites pass with a Release `kyty_emulator` build; a
    visible retest beyond 68 shaders is pending.

32. The blocker-31 retest cleared the clean partial-target transition. The latest run compiled 65
    shaders before rebinding the writable raw window at `0x60a5be5a00+0x2ffddf0` over the fully
    contained render target at `0x60a66d0000+0xff0000`. This target was no longer GPU-current and
    was already marked buffer-modified, so the native image was stale and guest/buffer memory was
    authoritative. Repeating that containing invalidation is therefore idempotent; rejecting it
    did not protect any current image bytes. `ClassifyBufferImageWrite` now accepts only this exact
    ownership state (render target, fully contained, `gpu_modified=0`, `buffer_modified=1`) and
    returns the existing render-target invalidation action. Partial repeated aliases and the
    contradictory GPU-current plus buffer-current state remain unsupported. Focused coverage uses
    the exact GTA ranges and verifies both negative cases. The Release emulator builds and the
    complete compute/host test executable passes; an in-game retest is pending.
33. The current log's remaining compute skips were decoded against the RDNA2 ISA before further
    implementation. `MUBUF 0x3f` is `BUFFER_ATOMIC_FMIN`, not a cache invalidation instruction.
    It now lowers through a 32-bit compare-exchange loop using the ordered `src < old` rule and
    returns the successful old value when GLC is set. This preserves the old operand for a NaN
    source, a NaN memory operand, and equal signed zeros, matching the hardware pseudocode. The
    exact skipped raw instruction (`hash=0x90394ea00`, `pc=0x160`) now decodes through the normal
    memory path. A Vulkan-executed regression covers the value and edge cases, SPIR-V validation,
    and returned-old behavior. `VOP3 0x14c` is the native-FP64 `V_FMA_F64` instruction and needs a
    real Float64 feature/type plus paired-register lowering; it must not be approximated as F32.
    `MIMG 0xe6` is `IMAGE_BVH_INTERSECT_RAY`, which belongs to the ray-tracing resource and
    capability track rather than the ordinary image path. `VOP3 0x14f` did not occur in this log
    and remains backlog-only. The Release emulator and full compute/host test executable pass.
34. The same current run repeatedly skipped compute shader `0x605ac68a00` because its fixed-offset
    `s_load_dword` at `pc=0x0c` read a dispatch-varying SRT address on a GPU-owned guest page. The
    addresses move by the shader record stride (`0xc40`) and fail before descriptor specialization,
    so this is not an unresolved scalar expression. `ReadShaderGuestDword` used
    `HostMemoryRangeIsReadable`, which rejects the memory tracker's intentional no-access
    protection before the read can fault, synchronize native contents, and restore guest access.
    The runtime SRT reader now follows the existing descriptor-validity contract and rejects only
    genuinely unmapped ranges with `HostMemoryRangeIsMapped`; the subsequent dword copy performs
    the tracked synchronization. Optional crash-diagnostic probes remain readability-gated and do
    not alter execution. The Release emulator and all eleven regression executables pass. The
    blocker-35 input log confirms the repeated `ReadConst pc=0x0c` skips disappeared.

35. The blocker-32/33/34 retests contain zero `CS skip` records, proving the prior render-target,
    atomic-FMIN, and SRT-reader changes cleared. The latest run compiled 59 shaders before the
    writable raw window at `0x6003673600+0x3af0bd60` fully contained the page-aligned 16-KiB image
    at `0x60225da000+0x4000`. `cached_kind=1` maps to `CachedImage::Kind::StorageTexture`, and the
    preceding `StorageTextureCache` Standard4KB detile records confirm this is a storage image,
    correcting the earlier sampled-image diagnosis. The image had already transitioned to buffer
    ownership (`gpu_modified=0`, `buffer_modified=1`), so its native storage image was stale and
    repeating the containing invalidation could not discard current image bytes.
    `ClassifyBufferImageWrite` now returns `InvalidateStorageTexture` when a non-exact raw window
    fully contains a page-aligned storage image that is not GPU-current. This covers the first
    clean invalidation and a repeat while buffer memory is authoritative. Exact raw and partial
    aliases, plus simultaneous GPU-plus-buffer ownership, remain unsupported. Focused regressions
    use the latest GTA ranges and both unsafe negative states. The Release emulator builds,
    `git diff --check` passes, and all eleven standalone regression executables pass; an in-game
    retest is pending.

36. The blocker-35 retest cleared the repeated storage-image transition and returned to the
    `0x60a5be0000` render-target allocation. The writable raw window at
    `0x60a5be1200+0x2fff8a0` begins `0x1200` bytes inside the page-aligned
    `0x60a5be0000+0x870000` target, covers the rest of that target, and continues beyond it.
    `cached_kind=2` is `CachedImage::Kind::RenderTarget`; `gpu_modified=0` and
    `buffer_modified=1` prove the native image was already stale after an earlier partial buffer
    write. This is the buffer-owned counterpart of blocker 31's clean partial overlap, not the
    fully containing geometry from blocker 32. `BufferCache` retains exact GPU-dirty byte ranges
    while merging owners, and `ObtainBufferForImage` publishes those ranges into coherent guest
    backing before rebuilding the full target. Repeating a page-or-larger partial invalidation is
    therefore idempotent. The classifier now admits that transition only for a page-aligned,
    non-GPU-current render target with at least one tracker page of real byte overlap. Sub-page
    edge contact and simultaneous GPU-plus-buffer ownership remain unsupported.

    The same range was first null-bound as a read-only raw descriptor. That was over-conservative:
    the image had already yielded ownership, so the cached buffer held the current bytes.
    `RegionInfo` now distinguishes native/clean image bytes from buffer-owned stale image bytes,
    and raw reads are coherent only when there are no GPU-current image bytes and every image
    overlapping GPU-dirty buffer bytes is already buffer-owned. Native-current images,
    dirty-buffer/clean-image contradictions, metadata boundaries, and page-only aliases retain
    their prior safety behavior. Focused cases reproduce the GTA range, sub-page and mixed-owner
    negatives, the read-coherence truth table, and Vulkan-backed region ownership. The Release
    emulator and all eleven standalone regression executables pass; an in-game retest is pending.

37. The same log exposed four unique compute skips. The two `MIMG 0xe6` records are the already
    deferred `IMAGE_BVH_INTERSECT_RAY` variants, and `VOP3 0x14c` remains the capability-gated
    `V_FMA_F64` track. The new `MUBUF 0x40` skip at compute hash `0x90394ea00`, PC `0x178`, is
    `BUFFER_ATOMIC_FMAX` per the RDNA2 ISA. The decoder, IR, resource tracking, and SPIR-V emitter
    now lower it through the same 32-bit compare-exchange framework as FMIN, replacing memory only
    for the ordered `src > old` relation and returning the successfully observed old value when
    GLC is set. This preserves the old operand for source NaN, memory NaN, and equal signed zeros.
    A Vulkan-executed regression covers all of those cases, SPIR-V validation, and returned-old
    behavior. The Release emulator and all eleven standalone regression executables pass.

38. The blocker-36/37 retest compiled 59 shaders and contains zero compute skips, read-only null
    aliases, unresolved imports, device-loss markers, or negative Vulkan results. This confirms
    the repeated partial-target/read-alias changes and `BUFFER_ATOMIC_FMAX` all cleared. The next
    writable raw descriptor covers only 24 bytes at `0x60a5c30e00`, wholly inside the clean,
    page-aligned render target at `0x60a5be0000+0x870000`. The target has authoritative guest
    backing (`gpu_modified=0`, `buffer_modified=0`), so invalidating its stale native view and
    recording those exact 24 buffer-dirty bytes is the same safe ownership transition as a larger
    partial write. `BufferCache` already tracks dirty bytes rather than treating a tracker page as
    wholly overwritten, and the next target use folds only those bytes into guest backing before
    reconstructing the full image.

    The former one-page minimum was therefore an arbitrary conservative threshold, not a data-
    coherence boundary. `ClassifyBufferImageWrite` now requires any real byte overlap for a clean
    or already buffer-owned, page-aligned, non-GPU-current render target. Byte-disjoint page-only
    contact remains excluded, as do GPU-current partial targets, unaligned partial targets, and
    contradictory GPU-plus-buffer ownership. Focused regressions reproduce the exact 24-byte GTA
    range, a sub-page edge crossing, the repeated buffer-owned case, and both GPU-current and
    unaligned negatives. The Release emulator, `git diff --check`, and all eleven standalone
    regression executables pass. The 76-shader blocker-39 input confirms this transition cleared.

39. The blocker-38 retest cleared the 24-byte ownership transition and compiled 76 shaders before
    `vkWaitForFences` returned `VK_ERROR_DEVICE_LOST` (`VkResult(-4)`) for queue 0, submission
    41362. The recorded operation was `DispatchDirect(6, 1, 1)` in mode 65 using compute shader
    `0x9039cff00`. Extracting that exact 492-dword shader from the saved ELF reproduced the compiler
    path offline: its 71-block, three-loop CFG repeatedly split a shared merge until it reached 142
    blocks, exhausted the split budget, and emitted a 25,698-word dispatcher fallback.

    The repeated split was not irreducible guest control flow. A conditional loop latch can branch
    directly to its containing loop's merge as a structured break. Treating that latch as a nested
    selection made both the loop and the selection claim the same merge, so each repair created the
    next synthetic conflict. Shared-selection splitting and final selection-merge reservation now
    recognize a conditional edge to the containing loop merge as loop control. The rule deliberately
    does not suppress conditionals targeting a loop continue/header; the existing shared-continue
    regression still requires their `OpSelectionMerge`.

    The exact GTA shader now structurizes at 71 blocks without splitting or dispatcher fallback.
    A focused synthetic regression covers a header exit and latch exit sharing the loop merge and
    requires `OpLoopMerge`, no dispatcher `OpSwitch`, and valid SPIR-V. The Release emulator and all
    eleven standalone regression executables pass. An in-game retest must confirm the shader logs
    `structured CFG success blocks=71` and that submission 41362 no longer loses the device.

40. The same input log exposed VOP3 opcode `0x14f` at compute shader `0x90395b000`, PC `0x164`.
    RDNA2 identifies it as `V_ALIGNBYTE_B32`. Decoder, IR, logging, and SPIR-V lowering now implement
    the low 32 bits of `(src0:src1) >> ((src2 & 3) * 8)`, preserving the existing `V_ALIGNBIT_B32`
    source ordering while masking the byte count to the ISA's four positions. Static decoder/IR
    coverage and a Vulkan-executed case (`0x11223344:0x55667788`, count 2 -> `0x33445566`) pass.
    The other newly observed skips are `V_FMA_F64` and two `IMAGE_BVH_INTERSECT_RAY` variants; they
    remain on their capability-gated tracks and are not approximated as F32 or ordinary image ops.

41. The blocker-39/40 retest no longer reported device loss or a `V_ALIGNBYTE_B32` skip. It produced
    71 shader-compile messages and then stopped updating the image while audio and controller-axis
    traffic continued. The emulator remained responsive, one CPU thread stayed busy, and working
    memory grew; all logged graphics/compute waits completed. The stable log has zero unresolved
    imports, fatal errors, negative Vulkan results, or `ReadConst` failures. Its remaining static
    capability skips are `V_FMA_F64` and the two `IMAGE_BVH_INTERSECT_RAY` variants.

    The last compiler record is compute shader `0x903957400`: 568 dwords decode to 351 instructions,
    43 CFG blocks and three natural loops, then structurize to 45 blocks. `IR LowerProgram` completed,
    but the next `SPIR-V EmitProgram` record never appeared. Extracting the exact shader from the
    saved ELF reproduced the stall without the game or Vulkan. The scalar-provenance work list
    reached a transient two-input phi for `s8` at the second loop header, then both inputs coalesced.
    Returning the lone value discarded the phi fixed-point boundary; a later visit recreated it, so
    the analysis alternated forever while interning symbolic values.

    Scalar provenance now retains an allocated phi's identity even when the current incoming set
    temporarily deduplicates to one value. It does not manufacture phis for previously unmerged
    invariant values. The exact shader now completes offline with 129 scalar values, six descriptor
    sources, six fixed SRT reads, five buffers, and one address resource. A sequential-loop
    regression guards bounded phi convergence, and normal compilation logs now bracket scalar
    provenance, SRT planning, resource tracking/materialization, and binding allocation so any future
    pre-SPIR-V stall identifies its stage. Release `kyty_emulator`, `git diff --check`, and all eleven
    standalone regression executables pass; the next in-game gate is completion of shader
    `0x903957400` and visible progress beyond the 71-message freeze.

    This run also exposed two separate structured-CFG gaps: shaders `0x605ac67400` and
    `0x605ac68a00` reported duplicate merge block 6 and successfully used 81,730- and 90,087-word
    dispatcher modules. They did not terminate this run, but their shared-merge shape remains a
    high-priority follow-up because such modules carry the same GPU-risk class as blocker 39. The
    162 image/metadata-boundary null reads and 16 oversized bindless-window warnings also remain
    bounded safety outcomes rather than this freeze's terminator.

42. The blocker-41 retest cleared the scalar-provenance freeze and reached 77 shader-compile
    messages. The new deterministic terminator is again compute shader `0x9039cff00`, but the
    blocker-39 dispatcher diagnosis is no longer applicable: the live compiler decoded all 289
    instructions, built 71 CFG blocks with three loops, structurized the shader to 72 blocks,
    emitted a 25,085-word module without dispatcher fallback, and created its Vulkan pipeline
    successfully. `vkWaitForFences` then returned `VK_ERROR_DEVICE_LOST` for queue 0, submission
    38748, after `DispatchDirect(6, 1, 1)` in mode 65.

    At this point Kyty classified the shader as wave32 because it incorrectly treated
    `COMPUTE_PGM_RSRC1` bit 30 as the wave-size selector. Blocker 47 later proves that bit is
    `MEM_ORDERED` and that this exact 64-thread queue/scan kernel is wave64. The generic defect found
    here remains valid for genuine wave32 shaders: lane indices for `V_READLANE_B32` and
    `V_WRITELANE_B32` had always been masked with 63, but wave32 selectors are modulo 32. Lane-index
    lowering now masks by the correctly decoded guest wave size.

    Second, a guest `GLC` buffer load was emitted as an ordinary SPIR-V `OpLoad`, even when used to
    poll memory modified through atomics. That loses the guest request to bypass the closest cache
    and re-observe globally coherent data, allowing stale values to keep the loop live. Shaders
    containing `GLC` storage-buffer accesses now mark the storage-buffer member `Coherent`; each
    `GLC` load uses the `Volatile` memory-access operand and is preceded by a device-scope
    acquire-release `UniformMemory` barrier. This is deliberately shader-local and leaves ordinary
    buffer loads unchanged.

    Generic regressions verify wave32 selectors use mask 31 while wave64 uses 63, and that a `GLC`
    storage-buffer load emits the coherent decoration, volatile load, memory barrier, and valid
    SPIR-V. The exact shader still compiles as a 72-block structured program and contains the new
    visibility path. Release `kyty_emulator`, `git diff --check`, and all eleven standalone
    regression executables pass. In-game validation is pending; if submission 38748 still loses the
    device, inspect the paired 32-bit lowering of the shader's 64-bit atomic exchanges before
    changing CFG or cache ownership again.

43. The blocker-42 retest progressed beyond the former 77-message terminator to 79 shader-compile
    messages and substantially later GPU work, so the lane-selection and `GLC` visibility
    changes were progression-positive. The same structured compute shader `0x9039cff00` was then
    dispatched as `DispatchDirect(3, 1, 1)` and lost queue 0 at submission 43708. The live compiler
    again decoded 289 instructions, structurized 71 CFG blocks and three loops to 72 blocks without
    fallback, and emitted a 25,105-word module. Pipeline creation succeeded; the negative fence
    result remained the first fatal signal.

    The follow-up audit found a concrete ISA violation in `BUFFER_ATOMIC_SWAP_X2`. The guest
    operation is one indivisible 64-bit exchange, but Kyty lowered it to two independent 32-bit
    `OpAtomicExchange` operations. Concurrent lanes or workgroups could therefore observe a torn
    pointer/queue pair and poison the scan protocol even though each half was individually atomic.
    The old comment claiming that such interleavings were legal was incorrect.

    `BUFFER_ATOMIC_SWAP_X2` now lowers to one `AtomicSwapU64` IR instruction and one 64-bit SPIR-V
    `OpAtomicExchange`. Shaders that use it declare `Int64` and `Int64Atomics` and receive a
    `uint64` storage-buffer view at the same descriptor binding as the existing dword view; both
    variables are decorated `Aliased`, the qword view uses an eight-byte stride, and emission checks
    qword alignment and runtime-array bounds. `GLC` returns the old low/high dwords from that single
    exchange. Vulkan device selection now requires and enables `shaderBufferInt64Atomics`, producing
    a deterministic unsupported-device result rather than creating an invalid pipeline.

    Static regression coverage verifies the dedicated IR opcode, exactly one native exchange for
    the x2 instruction, the two required capabilities, alias decoration, and SPIR-V validation. The
    Vulkan compute regression executes the exchange and verifies both the new 64-bit memory value
    and returned old pair. Release `kyty_emulator`, `git diff --check`, and all eleven standalone
    regression executables pass. In-game validation is pending; the next run must advance beyond
    submission 43708 and the 79-message checkpoint before a different blocker is named.

44. The blocker-43 retest again reached 79 shader-compile messages and lost queue 0 while waiting
    for submission 30689 after `DispatchDirect(6, 1, 1)` on `0x9039cff00`. This is the same visible
    checkpoint, not progression to a new shader. The live recompiler produced 24,705 words rather
    than the prior 25,105, confirming that the native 64-bit exchange path was active; pipeline
    creation succeeded before the device loss.

    Exact replay of the 492-dword, 289-instruction kernel then misclassified as wave32 found a
    separate structured-CFG defect. Its outer polling-loop header at guest PC `0x2a8` conditionally
    skips a `GLC` load and
    rejoins inside the loop; the actual loop exit occurs later. Kyty marked that guest block as the
    SPIR-V loop header and therefore emitted `OpLoopMerge`, but the same conditional also began an
    internal selection and had no `OpSelectionMerge`. SPIRV-Tools rejected the complete module with
    `Selection must be structured`. Disabling runtime shader validation allowed NVIDIA to create
    the pipeline but did not make the malformed control flow safe to execute.

    Structurization now splits a conditional natural-loop header when neither branch directly
    targets the loop merge. A synthetic empty block receives all preheader and backedge inputs,
    owns the loop merge/continue pair, and unconditionally enters the former header. Dominance,
    post-dominance, backedges, and natural loops are recomputed; the original conditional can then
    own its internal selection merge. Canonical loop headers whose condition directly exits to the
    loop merge remain unchanged. The exact replay grows from 72 to 73 structured blocks, stays off
    the dispatcher path, and its complete 24K-word SPIR-V validates. A focused rotated-loop test
    asserts the two-header shape, both merge instructions, and SPIR-V validity. Release
    `kyty_emulator`, `git diff --check`, and all eleven standalone regression executables pass;
    in-game validation is pending.

45. The blocker-44 retest confirms the loop-header repair is live but did not clear the execution
    terminator. Compute shader `0x9039cff00` decoded 289 instructions, built 71 blocks and three
    loops, structurized to 73 blocks without dispatcher fallback, emitted a 24,712-word module, and
    created its shader module and pipeline successfully. Queue 0 later lost the device while waiting
    for submission 27767 after `DispatchDirect(2, 1, 1)`, again at the 79-message checkpoint. The
    dispatch width and submission number continue to vary, so the stable identity is the same
    post-pipeline polling kernel rather than a fixed command index.

    Auditing that kernel's emitted memory model found the next concrete semantic gap. Its 32-bit
    `BUFFER_ATOMIC_ADD` ticket operation and both native 64-bit `BUFFER_ATOMIC_SWAP_X2` publication
    operations were emitted with SPIR-V `MemorySemanticsNone`. Kyty added a device-scope
    acquire-release barrier only *after* each atomic. That barrier can order later accesses, but it
    cannot make ordinary payload writes before an exchange part of the exchange's release sequence;
    nor can the atomic itself acquire payload published by another workgroup. This leaves the exact
    queue/scan protocol under-specified even though the exchanged qword is indivisible and its GLC
    polling loads are volatile.

    Storage-buffer atomic read/modify/write operations now carry device-scope acquire-release
    `UniformMemory` semantics directly on the SPIR-V atomic instruction. The change covers both the
    generic 32-bit atomic path and the native uint64 exchange while retaining the existing post-
    atomic barrier. Binary-level regressions resolve the memory-semantics constant operand and
    require `0x48` on both `OpAtomicIAdd` and the buffer `OpAtomicExchange`; SPIR-V validation, the
    complete shader CFG suite, and the Vulkan compute suite including the qword exchange pass.
    Release `kyty_emulator`, all eleven standalone regression executables, and `git diff --check`
    pass; an in-game retest remains pending.

46. The blocker-45 retest again reached 79 shader-compile messages and lost queue 0, now at
    submission 26839 after `DispatchDirect(41, 1, 1)`. The live `0x9039cff00` module still decoded
    289 instructions, built 71 CFG blocks with three loops, structurized to 73 blocks without
    fallback, emitted 24,712 words, and created its pipeline. This is a negative result for the
    ordered-atomic change, not a regression to an earlier CFG or split-atomic blocker. The dispatch
    width has now varied through 2, 3, 6, and 41 while the shader identity remains stable.

    Exact decode shows a decoupled-lookback prefix scan. Lane zero allocates a logical ticket with
    `BUFFER_ATOMIC_ADD`; each workgroup publishes a state/value pair with a native
    `BUFFER_ATOMIC_SWAP_X2`; later workgroups poll preceding pairs with two
    `GLC BUFFER_LOAD_DWORDX2` sites until state becomes nonzero, accumulate state-1 partials, then
    terminate on state 2 and publish their final pair. Blocker 45 correctly establishes release/
    acquire ordering on the atomic publications, but the poll remains two ordinary volatile dword
    loads and the runtime log does not expose the four materialized descriptor ranges. Blocker 46 is
    therefore a bounded descriptor/range and qword-polling audit: first prove resource identity,
    alignment, and bounds, then use a single uint64 acquire load only if that guest contract is
    established. Preserve the native exchange, wave-size selector, loop-header, and ordered-atomic
    repairs while testing this path.

    The `context.cpp:540` location is only the fence-result guard. That file contains 14
    `EXIT_NOT_IMPLEMENTED` sites, almost all wrapping Vulkan failures or internal invariants; they
    are not 14 features to implement. The current log establishes one mandatory fix to advance and
    four other visible implementation families: duplicate-merge CFG structurization, bindless
    range provenance/materialization, native FP64 FMA, and BVH ray intersection. The complete known
    inventory is therefore five families, but only the polling kernel is proven to block current
    Story Mode progress. CFG and bindless work may become pre-playable correctness requirements;
    FP64 and BVH remain deferred for the offline non-RT target. A precise total to playability cannot
    be inferred until execution reaches player control and exposes later runtime paths.

    A second run of the same pre-blocker-46 binary reported 77 shader-compile messages and lost
    logical queue 1 at submission 41193 after `DispatchDirect(6, 1, 1)`. The stable evidence is still
    shader `0x9039cff00`, 289 decoded instructions, 73 structured blocks, 24,712 SPIR-V words, a
    successfully created pipeline, and the negative fence result. This confirms that shader count,
    queue number, submission sequence, and dispatch width are supporting context rather than a
    regression or progress metric.

    The bounded audit found that `BUFFER_LOAD_DWORDX2` is split into two component-level
    `BufferLoadDword` IR instructions. Even with `GLC`, the polling pair was therefore observed by two
    separate volatile 32-bit loads, while the producer publishes the exact same tracked descriptor
    resource through one native `AtomicSwapU64`. The emitter now reconstitutes only that matched,
    untyped, two-component GLC operation. Component zero emits a uint64 `OpAtomicLoad` with device-
    scope acquire `UniformMemory` semantics (`0x42`), bitcasts and stores both dwords; component one
    is covered. Runtime alignment and uint64-array length guards drop invalid pairs to zero. Typed,
    formatted, non-GLC, unmatched-resource, and ordinary dword loads are unchanged.

    Binary tests require exactly one 64-bit atomic load with semantics `0x42`, retain the ordinary
    GLC load path when no qword publisher matches, and validate the module. The Vulkan regression
    performs `BUFFER_ATOMIC_SWAP_X2`, reacquires the new pair through `BUFFER_LOAD_DWORDX2 GLC`, and
    verifies both published dwords. The Release emulator builds, `git diff --check` passes, and all
    eleven standalone regression executables pass. Blocker 46 is code-complete pending an in-game
    retest.

47. The blocker-46 retest reached 81 shader-compile messages, the furthest count recorded for this
    checkpoint, but `0x9039cff00` still lost queue 0 at submission 58380 after
    `DispatchDirect(6, 1, 1)` in mode `0x41`. The 24,228-word module proves that the matched qword
    acquire path was active, compared with 24,712 words before blocker 46, so that fix is retained as
    progression-positive but insufficient. The user also heard audio stop near the 68-message
    checkpoint. The log contains no AJM or SDL error: both SDL endpoints opened, and AJM instance
    decode batches continued until immediately before the final shader compilation and fence
    failure. Treat the audible stop as a downstream game/GPU scheduling symptom unless a later log
    supplies an audio-subsystem failure.

    The exact AGC register state identifies the remaining concrete defect. The shader requests
    `64x1x1` threads. Kyty interpreted `COMPUTE_PGM_RSRC1` bit 30 as `W32_EN`, but that bit is
    `MEM_ORDERED`; the compute wave32 selector is
    `COMPUTE_DISPATCH_INITIATOR.CS_W32_EN` bit 15. Dispatch mode `0x41` has bit 15 clear, so this
    shader is wave64. Its explicit lane-31/lane-63 and high-half `exec` operations independently
    confirm that result. Running it as two unrelated host wave32 subgroups breaks ballot, shuffle,
    DPP, read-lane, and read-first-lane communication across the guest wave and can deadlock the
    lookback protocol.

    PM4 decode now derives compute wave size from the dispatch initiator rather than program
    resource bit 30. On hosts with subgroup size 32, a bounded workgroup-backed wave64 path handles
    exactly one 64-thread guest wave: a separate 66-dword workgroup scratch array, workgroup barriers,
    atomic ballot accumulation, and shared lane exchange implement the cross-half operations without
    aliasing guest LDS. Multi-wave workgroups remain rejected because a workgroup barrier cannot
    safely model independent guest-wave synchronization. Static SPIR-V coverage verifies the
    scratch/barrier/atomic path and forbids subgroup ballot/shuffle in this mode. A Vulkan runtime
    regression enables only guest lane 63, performs `V_READFIRSTLANE_B32`, and proves that all 64
    invocations observe lane 63. The Release emulator builds, `git diff --check` passes, and all
    eleven standalone regression executables pass. In-game validation is pending.

The same build also closes the three called static-import groups identified by the first Ghidra
campaign. `ziVA3whp3p4` is registered as the alternate AGC rewind export proven by its get-size,
packet-write, and initial-state caller sequence. The five recovered NpUtility handlers preserve the
guest's signed-out lifecycle, and the three bounded VideoRecordingP handlers select GTA's explicit
recorder-unavailable path. These are deliberately not title-ID shims: each handler validates the
recovered library ABI and applies a generic offline platform policy.

The post-merge audit was expanded beyond the observed crashes. It compared all nine conflict files,
the pre-merge texture-cache method inventory, every named test in both shader test programs, and all
three local stash snapshots:

- The newest GTA snapshot (`stash@{0}` when audited) is based on `a182c47` and contains 37 changed
  files. It is the only stash containing uncommitted GTA work immediately before the upstream merge.
- The two older Hades snapshots are based on `f743a48` and `147188b` and contain 36 and 24 changed
  files. Their bases are successive ancestors of the GTA snapshot, and their compatibility work was
  subsequently committed into the Hades/GTA lineage before the merge.
- None of the three stash commits has a third parent, so no untracked-file payload is hidden outside
  the normal snapshot and index trees. The stashes remain intact as recovery references; applying
  any of them wholesale would overwrite the upstream cache split and later blocker fixes.

Every distinctive added line and function was classified as exact, refactored, superseded, or
missing. Configuration, emulator, host-memory, loader, resource-tracking, and most recompiler files
are exact or retain the same behavior. The upstream generalized APIs correctly subsume the removed
per-kind cache entry points: `QueryRegion` replaces the old page/range and metadata predicates,
`ClearImageFromBuffer` replaces the three aspect-specific clear methods, `ImageOps` owns resource
destruction, sampled target views come from the split image-view cache, and sparse overlap
resolution replaces `RetireStoragePageNeighbors`. The old render-target-only batch helper was
intentionally superseded by blocker 21's page-isolated sampled-image/render-target batch classifier.
All 427 named cases from the newest shader regression snapshot are present. Three apparent name
differences in the older snapshots are the same image-clear checks consolidated under
`ComputeImageClearRuntimeShape`; no pre-merge test case is absent.

Blockers 24 through 26 exposed a limitation of that inventory-level audit: a named test can survive
while one of the production predicates it should constrain is narrowed during conflict resolution.
The storage regressions now assert the shared predicates used by descriptor admission, cache creation,
and readback classification, so those layers cannot silently diverge again for the Standard4KB
layout.

### Commit strategy for the audited worktree

The current tree should not be committed as one undifferentiated snapshot, but the graphics work
also must not be split file-by-file. Its IR fields, materialization rules, SPIR-V address emission,
descriptor bindings, cache ownership transitions, and regression expectations form one tested
contract. The safe local commit sequence is:

1. `loader: export SELF and import inventory for analysis`: the `--save-elf` option and only its
   loader/emulator/CLI hunks. Extraction refuses input/output aliasing and pre-existing outputs.
2. `graphics: support dynamic compute resources and coherent aliases`: the compute skip option,
   dynamic/bindless resource pipeline, storage and render-target layout restorations, DCC state,
   image/buffer ownership transitions, read-only coherent prefixes, and completed cross-queue
   buffer retirement, corrected dispatch wave-size decode, and bounded workgroup-wave64 emulation,
   together with the CMake and regression changes. Keep this atomic unless each proposed
   subdivision is independently built and all eleven executables are rerun.
3. `libs: implement observed offline platform contracts`: the AGC rewind alias, five NpUtility
   handlers, three VideoRecordingP handlers, and their registration plumbing.
4. `docs: update GTA V bring-up record`: this document after the code commits, so every recorded
   behavior points at code already present in history.

Do not stage `_DownloadData/`, `_TempData/`, the external `gtav-work` Ghidra project, extracted
ELFs, import inventories, logs, shader/pipeline dumps, or game paths. Preserve the newest GTA stash
until the four commits have been built from a clean index and the later in-game retest has passed;
the two older Hades stashes can be kept as historical recovery points but must not be applied to
this branch.

The next upstream range was integrated in isolation on 2026-07-19. The audited GTA work was first
split into commits `31467bb`, `1b4f1b4`, `7163ff6`, and `3e9e0b9` on
`feature/gtav-compatibility`; that branch remains fixed at `3e9e0b9` as the pre-refactor fallback.
`feature/gtav-upstream-bd9086e` was then created at the same tip and merged with `upstream/main` at
`bd9086e`. The upstream range removes Vulkan SDK coupling, migrates the backend to Vulkan-Hpp,
centralizes transfer helpers, and touches 95 files. It still does not directly implement blocker
47's dispatch-initiator wave-size decode or workgroup-wave64 path, so any in-game improvement must
be demonstrated rather than assumed.

The isolated merge produced 21 textual conflicts concentrated in the renderer and shader
regressions. Resolution retained the upstream Vulkan-Hpp command-buffer, image, transfer, and
lifetime APIs while porting the GTA cache layouts, metadata ownership, compatible image views,
recording-generation tracking, dispatch diagnostics, and workgroup-wave64 implementation onto
them. The compiler audit also restored pre-refactor behavior that a textual merge could silently
drop: format-usage-aware texture selection, shader-address write guards, the texture-descriptor
address updater, draw/compute address-write propagation, DCC initialization/readback through the
new transfer API, and the GTA-specific regression cases. A stale `storage-mip` fatal expectation
was removed because dynamic storage mips are now intentionally admitted and validated by their
dedicated descriptor death cases.

The merged Release `kyty_emulator`, `shader_cfg_tests`, and
`shader_recompiler_compute_tests` build successfully against the refactored backend. All eleven
standalone regression executables pass, including the Vulkan workgroup-wave64 case; both
`git diff --check` and the staged-diff check are clean. `_DownloadData/`, `_TempData/`, and all
three stashes remain outside the merge. The integration branch is ready for a like-for-like Story
Mode run, but it does not replace the fallback branch until that test advances or at least matches
blocker 47 without a new regression.

That audit did expose one intentionally incomplete merge bridge unrelated to the immediate R8
assertion: `ConsumeVideoOutDccMetadataTransfer` returned `false`, and registration/acquisition had
lost the pre-merge compressed-surface content state. The complete generic behavior is restored on
top of the sparse image index: display and render metadata planes are validated and registered,
uniform whole-surface clear metadata initializes native contents, coherent/guest/native state and
clear-code identity are tracked, full-overwrite and preserve/present ownership rules are enforced,
GPU writes acquire both DCC planes, and the recognized render-to-display metadata transfer retains
the native-current image. This removes the last known conservative cache-split bridge while keeping
upstream's resource lifetime and image-owner registration.

Open items after the 2026-07-19 build: retest blocker 47 with the same quiet launch. Confirm the log
selects `workgroup cross-lane emulation` for `0x9039cff00`, retains its 73-block structured CFG, and
advances beyond the device-losing polling dispatch. Do not restore the split 32-bit approximation,
weaken atomicity, broaden the qword-load matcher, force this kernel to wave32, or relax texture-cache
ownership without new evidence. The remaining capability backlog is
`VOP3 0x14c` (native FP64 paired-register FMA) and two `MIMG 0xe6` BVH variants; none may be
approximated as an ordinary F32/image operation. Preserve the oversized-window size guard and
identify bad contributors before changing it. Record the first subsequent deterministic failure or
visible progress; treat shader compilation count as supporting context, not the progress metric.

The post-Ghidra Release build completes and all eleven standalone regression executables pass.
The next game run should no longer report the observed `Fc8qxlKINYQ`, NpUtility,
`ziVA3whp3p4`, or `MUBUF 0x3f` skips. If a queue-overlap diagnostic reports
`completed_queues=0`, treat that as a live scheduler dependency; do not relax the cache ownership
rule.

## Static implementation inventory

Ghidra is useful for turning CPU-side imports and their reachable callers into a planned backlog,
but it cannot replace execution-driven graphics bring-up. Shader variants, PM4 streams, descriptor
contents, resource aliases, page ownership, queue synchronization, and streamed assets are created
or selected at runtime and must remain driven by bounded emulator telemetry.

The current non-versioned analysis campaign uses this repeatable pipeline:

1. Kyty's `--save-elf` mode converts the locally owned SELF to an analysis ELF and exits without
   executing guest code. It writes a separate import-symbol inventory and omits only auxiliary ELF
   payloads that are not present in the SELF. No extracted binary or symbol inventory belongs in
   the repository.
2. Import the ELF into a per-version local Ghidra project. Run bounded automatic analysis so a
   timeout still leaves a useful, saved partial database.
3. Parse the Sony module, library, symbol, PLT, and GOT metadata. Bridge both PLT calls and direct
   RIP-relative GOT calls because stock Ghidra does not resolve all Sony relocations.
4. Join imported NIDs against Kyty registrations and focused semantic-test coverage. A NID literal
   appearing in source means only that a handler is registered; it does not prove correct ABI,
   lifecycle, callback, error, or synchronization behavior.
5. Rank evidence in this order: called on the recorded offline path, statically reachable from that
   path, imported but not observed, and online-only capability outside the initial target.
6. Decompile only prioritized callers to recover argument use, return-value checks, object lifetime,
   callback ordering, and fatal/error branches. Implement generic library behavior and retain a
   focused regression before advancing to the next observed blocker.
7. Refresh the inventory whenever the application version or executable changes.

The refreshed pass found 1,301 imports across 55 libraries and 52 modules. After the bounded
implementations above, 613 NIDs appear in Kyty source and 688 do not. This is deliberately an
upper bound, not a promise that 688 functions
must be implemented: 437 missing imports belong to the online-focused `libSceNpCppWebApi`, which is
outside the initial Story Mode target unless runtime evidence proves otherwise. The largest
remaining groups are libc (86), AGC (79), AGC driver (18), video recording (9), commerce (7),
content search (6), and several small NP/dialog/content modules. Raw inventories and call-site
addresses remain in the non-versioned work directory.

The relocation bridge records 119,241 non-empty static caller rows across 1,038 unique NIDs. Of
those, 596 do not currently appear registered in Kyty, including 437 from `libSceNpCppWebApi`.
Static reachability is still not a playability count: the latest offline run called zero unresolved
imports, so none of those 596 is a proven current blocker.

Implementation priority is therefore:

- P0: imports actually called during offline Story startup. The observed video-recording,
  NP utility, and AGC gaps are now bounded; promote another import only when runtime evidence or a
  complete reachable caller contract establishes its ABI and required offline behavior.
- P1: reachable local-runtime gaps in libc, kernel, AGC/driver, audio, video, user, save, and system
  services.
- P2: graceful offline behavior for dialogs, content capabilities, recording, invitations, and
  entitlement queries that Story Mode probes but does not require.
- P3: online APIs, commerce flows, and production-service authentication. Keep them disconnected
  and out of scope until the offline success ladder is complete.

The bounded Ghidra pass saved a useful partial database after its analysis time limit. The custom
Sony relocation bridge recovered more than 119,000 import call sites and matched the recorded
video-recording and NP utility return addresses, validating the static/dynamic join. Indirect
function-pointer calls may still require runtime caller telemetry or targeted data-flow analysis.

### Observed `VideoRecordingP_v1` import

The NID `Fc8qxlKINYQ` has seven direct call sites in this executable, all through the same PLT
entry. Its recovered ABI is consistent with `int function(uint32_t command, const void* data,
size_t size)`. The three calls observed during startup use commands `6`, `2`, and `0x0a01`, pass
stack-built UTF-16 strings of 0x24, 8, and 0x44 bytes, and discard the return value. A later caller
again sends command `6` with a UTF-16 string, and a cleanup path sends command `2` before releasing
manager-owned resources.

The remaining callers establish that the NID is more than a metadata setter. Commands `0x0a004`
and `0x0a006` are issued with null data and their signed return values drive recording-manager
state transitions. A sibling video-recording import is first called with command `0x0a003`, an
eight-byte output location, and size 8. The caller explicitly handles video-recording failures
including out-of-memory, fatal, and no-space results. This supports a command-multiplexed recording
control interpretation, although no reliable public symbol-name mapping for this private NID was
found.

The bounded implementation registers `Fc8qxlKINYQ`, query NID `sA6+5XdbqMA`, and capability-probe
NID `ZvWzS2wTIMc`. It accepts only the observed even-sized UTF-16 metadata commands, validates and
zeros the recovered four- and eight-byte query outputs, and returns the explicitly handled
`0x80a80003` unsupported result for capture controls and the capability probe. That prevents GTA
from allocating recorder buffers while preserving its normal offline-disabled state. The other six
imports remain unregistered until their initialization, callback, and ownership contracts are
recovered; proximity in the PLT is not ABI evidence.

### Observed `NpUtility_v1` worker

Ghidra recovered the complete worker at `0x02a89eb0` and its four-entry jump table. NIDs
`hqzi1IHdQQQ` and `mA0zsbqm+kA` both receive a 0x38-byte input whose first qword is its size and a
timeout of 8,000,000 microseconds, then return a signed request ID. `BYIZGKm6bO4` receives that ID
and a four-byte state pointer; state 1 sleeps and polls again, state 0 takes the failure cleanup,
and another state retrieves the result. `kvdMF48mB3Y` aborts the request, while `pLr1fEQS1z8`
receives the ID and a zero-initialized 0x18-byte result containing two doubles and a signed result
code. The two successful phases publish one converted double apiece before the worker signals
completion.

Kyty registers exactly those five call shapes. Because NpManager already reports the local user as
signed out, both starts return the shared `0x80550006` signed-out error after pointer/size
validation. Poll, abort, and result handlers validate their outputs and report request-not-found
when no request can exist. This is intentionally an offline failure contract, not a fabricated
asynchronous success path or network service.

### Observed `Agc_v1` rewind import

The repeatedly called NID `ziVA3whp3p4` has one recovered direct wrapper and a stable two-argument
shape. Its caller obtains `GraphicsDcbRewindGetSize()`, writes an indirect-buffer packet immediately
after that reserved prefix, then invokes the NID with the command-buffer base and
`initial_state=1`. This is the complete `GraphicsDcbRewind` producer contract already implemented
and consumed by Kyty's strict two-dword `IT_REWIND` handler, not a destructor or resource-release
operation. The NID is now registered as an alternate export of `GraphicsDcbRewind`; both defined
initial-state values retain the existing packet validation regression.

The neighboring wrapper writes the indirect-buffer packet through the already registered
`Ikfdt-rIqCE` path before the rewind prefix is finalized. Its opaque source name remains a separate
inventory concern, but its return is not the unresolved call seen in the run. Other missing AGC
imports must still be classified by call shape and packet effect rather than aliased by proximity.

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
