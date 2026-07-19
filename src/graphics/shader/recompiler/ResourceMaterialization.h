#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

constexpr uint64_t FlatAddressWindowSize = 0x10000;

// Storage-buffer bindings start at the descriptor base aligned down to this boundary; the
// low-bits remainder is delivered to the shader through the flattened SRT and re-added to every
// access. 256 is the Vulkan ceiling for minStorageBufferOffsetAlignment, so aligned-down bases
// satisfy any device.
constexpr uint64_t StorageBufferBindAlignment = 256;

// Flattened-SRT layout: [srt.reads][one bind-remainder dword per buffer][bindless window bases].
uint32_t BindlessWindowCount(const Program& program);

struct ResourceSnapshot {
	struct Address {
		uint64_t guest_base   = 0;
		uint64_t binding_base = 0;
		// Explicit binding span; zero lets the renderer probe forward from binding_base.
		uint64_t binding_size = 0;

		bool operator==(const Address& other) const = default;
	};

	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<Address>         addresses;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
};

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot,
                              std::string* error);
bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot,
                                    std::string* error);

// Resolves the immutable dense resource topology against one runtime user-data/SRT snapshot.
// On failure the destination is unchanged.
bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot* snapshot, std::string* error);

// Applies runtime descriptor shape/format facts to a copied dense topology before layout and
// emission. On failure the program is unchanged.
bool SpecializeResources(Program* program, const ResourceSnapshot& snapshot, std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
