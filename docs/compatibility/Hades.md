# Hades compatibility notes

This document records the source snapshot used to reach gameplay in a legally obtained copy of *Hades*. It is a compatibility report, not a claim of complete or stable emulation.

## Tested version and source

- Title: *Hades*
- PlayStation title ID: `PPSA03355`
- Tested game version: `01.003.000`
- Working source commit: `17f508ae8651e5b87bdd42bbbe43d054a850b577`
- Working snapshot branch: `archive/hades-working`
- Review branch: `feature/hades-compatibility`
- Upstream base commit: `13499502c909544244151823a838aee7b77b94a5` (`Support raw buffer writes to sampled textures`)
- Test date: 2026-07-17

The working source commit is the exact code snapshot that was used for the successful gameplay test. The review branch reorganizes that work into focused technical commits and removes incidental formatting noise.

## Current status

| Area | Observed status |
| --- | --- |
| Boot | Boots through the title screen and main menu. |
| Gameplay | Opens Options and Play, reaches the introduction, starts a new game, and reaches active combat. |
| Graphics | Major menus, portraits, environments, characters, and effects render. Some text, textures, and other assets can still be missing or incorrect. Earlier severe menu corruption is substantially improved. |
| Performance | Generally responsive during menus and exploration on the tested machine, with significant frame-rate drops during combat and shader compilation. Performance is not yet representative of a finished emulator. |
| Input | Keyboard navigation and gameplay input work; a physical controller was not required for this test. SDL-compatible controllers are also supported by the existing input path. |
| Audio | Music, dialogue, and other audio were audible during the tested path. |
| Saving | Save-slot UI and local save-data setup are reached. Persistence after emulator restart and longer-term save compatibility have not been fully verified. |
| Stability | No deterministic crash was observed in the latest short gameplay test. Earlier crashes involving render-target image views and buffer/metadata aliasing were reproduced and addressed, but extended play-through stability is unknown. |

## Technical changes

The compatibility work covers generic emulator behavior rather than a title-specific bypass:

- Corrected guest-entry inline-assembly clobber declarations so the compiler cannot reuse scratch registers before guest stack and frame operands are consumed.
- Made host thread IDs and guest affinity state atomic, and applied guest CPU affinity to Windows host threads after creation and when changed later.
- Added an opt-in minimum latency for ordinary file reads. The default remains zero; the tested command used `100` microseconds to avoid unrealistic host I/O timing.
- Made controller disconnect handling tolerate duplicate or stale SDL disconnect events.
- Restored the small Vulkan C enum-string surface Kyty uses when building against Vulkan-Headers versions that no longer ship the old helper.
- Corrected shader resource analysis, SPIR-V export handling, control-flow emission, and raw 64-bit value movement. Preserving raw VCC/EXEC bits was important for correct glyph and UI shader behavior.
- Validated mapped AGC vertex metadata before copying it, preventing invalid guest pointers from becoming host reads.
- Expanded image layout and view compatibility checks, resource footprint calculations, cached image-view handling, and render-target view selection.
- Synchronized buffer, texture, render-target, and metadata aliases without aborting on legitimate overlapping resource layouts. Address-backed buffer views are clipped at incompatible image or metadata pages.
- Added compute and draw resource preparation, descriptor binding, color-write/blend state handling, framebuffer attachment filtering, and video-out acquisition paths needed by more complete workloads.
- Added regression coverage for shader recompilation, metadata parsing, image overlap resolution, address-buffer clipping, resource tracking, and related graphics paths.

## Test system

- Operating system: Windows 11, build `10.0.26200.0`
- CPU: AMD Ryzen 7 9800X3D 8-Core
- GPU: NVIDIA GeForce RTX 5090
- NVIDIA driver: `610.62`
- Graphics API: Vulkan

Results on other drivers, GPU vendors, or operating systems may differ.

## Launch flags

From the repository root after building the Windows target:

```powershell
.\_Build\windows\kyty_emulator.exe `
  --game "<path-to-a-legally-obtained-Hades-dump>" `
  --file-read-min-latency-us 100 `
  --shader-log-direction Silent `
  --graphics-debug-dump false `
  --printf-direction Silent
```

The game path is deliberately a placeholder. No game path or title-specific location is required by the source changes.

## Keyboard controls used

| Keyboard | Emulated control |
| --- | --- |
| `W`, `A`, `S`, `D` | D-pad up, left, down, right |
| `J` | Cross |
| `I` | Triangle |
| `K` | Square |
| `L` | Circle |
| `Q`, `E` | L1, R1 |
| `Enter` | Options |
| `Backspace` or `Tab` | Touch pad |
| `Escape` | Exit emulator |
| `Space` | Pause emulator |

## Reproduction procedure

1. Build `kyty_emulator` on Windows from the working source commit.
2. Launch a legally obtained, locally dumped copy of the tested game version with the flags above.
3. Wait for initial shader compilation and reach the main menu.
4. Open and close Options, then open Play.
5. Select or create a save slot and continue through the introduction.
6. Reach the first playable area and exercise movement, menu, dialogue, and combat input.
7. Continue into active combat and observe graphics, audio, stability, and frame pacing.
8. Repeat the same scene after shader warm-up when comparing performance.

## Validation performed

The working tree was built with the Windows CMake/Ninja configuration. The following regression executables passed during development:

- `shader_recompiler_compute_tests.exe`, including the focused `--image-overlap-only` path
- `resource_tracking_tests.exe`
- `shader_vertex_metadata_tests.exe`

The `kyty_emulator` target and the test targets were rebuilt after the final source fix. These unit-style tests do not replace in-game validation.

## Known limitations and follow-up work

- Some glyphs, text surfaces, texture layers, and UI/game assets remain absent or incorrectly composed.
- Combat can cause substantial frame-rate drops, especially while new shaders are compiled.
- Long sessions, all save/reload paths, suspend/resume, and game completion have not been tested.
- More capture-driven investigation is needed around texture alias synchronization, metadata ownership transitions, render-target view compatibility, and shader resource layouts.
- The conservative address-view clipping used around image or metadata holes should receive broader regression coverage and performance measurement.

## Scope of the compatibility changes

The retained source contains no hardcoded Hades title ID, shader hash, frame number, guest address, local game path, or executable patch. The changes implement generic runtime, shader, render-state, resource-tracking, texture-alias, presentation, and input behavior. No game files were modified.

No current source change is intentionally gated to Hades. Some newly supported paths are conservative while the emulator's resource model is still being expanded; those paths should be reviewed against additional titles before an upstream proposal.

## Media and legal hygiene

Local screenshots were used to verify the title screen, Options menu, Play/save menu, introduction, dialogue, and gameplay. They are intentionally not committed because they contain copyrighted game artwork. If an authorized public capture is published later, link it from this section rather than placing game media in the source repository.

This repository must not contain game data, decrypted executables, keys, firmware, proprietary Sony libraries, shader dumps derived from a game, save data, RenderDoc captures containing game resources, or copyrighted system files. Generated directories for those local-only artifacts are ignored. Only emulator source, generic tests, and documentation are included in these branches. KytyPS5 remains licensed under GPL-2.0-only.
