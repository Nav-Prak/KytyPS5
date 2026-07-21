#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCANDIAG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCANDIAG_H_

#include <cstdint>

// Temporary diagnostic for the 0x9039cff00 decoupled-lookback state-clear hunt. Records recent
// GPU/CP buffer writes to a ring so the scan's buf[2] bind can report which shader or CP op last
// wrote that exact guest memory. tag: shader hash for compute/graphics writes, 1 = CP WriteData,
// 2 = CP FillBuffer. Remove together with the ScanBufferBind diagnostic once the missing clear is
// understood.
namespace Libs::Graphics {

void ScanDiagRecordWrite(uint64_t addr, uint64_t size, uint64_t tag, uint32_t value);
void ScanDiagDumpOverlaps(uint64_t addr, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCANDIAG_H_
