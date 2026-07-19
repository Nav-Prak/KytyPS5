#include "graphics/shader/recompiler/ResourceMaterialization.h"

#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/shader/recompiler/BufferFormat.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

Decoder::ImageDimension DescriptorDimension(const DescriptorValue& descriptor) {
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube:
		case Prospero::ImageType::kColor1DArray:
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaaArray: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor1D:
		case Prospero::ImageType::kColor2D:
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2D;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor) {
	return ((descriptor.dwords[3] >> 28u) & 0x8u) != 0;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

uint32_t DescriptorPackedStride(const DescriptorValue& descriptor) {
	const auto stride       = (descriptor.dwords[1] >> 16u) & 0x3fffu;
	const auto swizzle      = (descriptor.dwords[1] >> 31u) & 1u;
	const auto index_stride = (descriptor.dwords[3] >> 21u) & 3u;
	const auto add_tid      = (descriptor.dwords[3] >> 23u) & 1u;
	return stride | (swizzle << 14u) | (index_stride << 16u) | (add_tid << 20u);
}

uint32_t DescriptorBufferFormat(const DescriptorValue& descriptor) {
	return (descriptor.dwords[3] >> 12u) & 0x7fu;
}

uint64_t AddressSpecialization(const AddressResource&           resource,
                               const ResourceSnapshot::Address& snapshot) {
	return resource.kind == ResourceKind::Flat || resource.source == ScalarProvenance::Unknown
	           ? snapshot.binding_base
	           : snapshot.guest_base - snapshot.binding_base;
}

constexpr uint64_t MaxBindlessWindowBytes = 1ull * 1024ull * 1024ull * 1024ull;
constexpr uint64_t MaxBindlessTableBytes  = 64ull * 1024ull * 1024ull;

bool ReadGuestDwords(const SrtRuntime& runtime, uint64_t address, uint32_t* dst, uint32_t count) {
	if (runtime.read_memory != nullptr) {
		for (uint32_t i = 0; i < count; i++) {
			if (!runtime.read_memory(runtime.userdata, address + i * 4ull, dst + i)) {
				return false;
			}
		}
		return true;
	}
	std::memcpy(dst, reinterpret_cast<const void*>(address), count * sizeof(uint32_t));
	return true;
}

bool GuestRangeReadable(const SrtRuntime& runtime, uint64_t address, uint64_t size) {
	if (size == 0 || address > AddressMask || size - 1u > AddressMask - address) {
		return false;
	}
	if (runtime.read_memory == nullptr) {
		// Live-memory evaluation: uninitialized descriptor heaps contain arbitrary bytes, so an
		// unmapped base is the strongest signal that a table entry is not a real descriptor.
		// Mapped-but-protected pages count as valid: the memory tracker protects GPU-owned pages
		// and a CPU touch resolves through the page-fault handler.
		return HostMemoryRangeIsMapped(address, size);
	}
	uint32_t   ignored = 0;
	const auto last    = (address + size - 1u) & ~uint64_t {3};
	return runtime.read_memory(runtime.userdata, address & ~uint64_t {3}, &ignored) &&
	       runtime.read_memory(runtime.userdata, last, &ignored);
}

bool DecodeBindlessRawBuffer(const SrtRuntime& runtime, const uint32_t* entry, uint64_t* begin,
                             uint64_t* end) {
	// Buffer descriptors have TYPE=0. Reject texture prefixes and arbitrary record payloads before
	// they can enlarge the synthetic binding window.
	if ((entry[3] >> 30u) != 0) {
		return false;
	}
	const auto base   = (static_cast<uint64_t>(entry[1] & 0xffffu) << 32u | entry[0]) & AddressMask;
	const auto stride = (entry[1] >> 16u) & 0x3fffu;
	const auto records = entry[2];
	const auto bytes =
	    stride == 0 ? static_cast<uint64_t>(records) : static_cast<uint64_t>(stride) * records;
	if (base == 0 || bytes == 0 || bytes > MaxBindlessWindowBytes ||
	    !GuestRangeReadable(runtime, base, bytes)) {
		return false;
	}
	*begin = base;
	*end   = base + bytes;
	return true;
}

void WarnBindlessWindow(uint32_t first_use_pc, const std::string& reason) {
	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
		LOGF("bindless window at pc 0x%08" PRIx32
		     " degraded to a null binding (accesses will be dropped): %s\n",
		     first_use_pc, reason.c_str());
	}
}

// GTA's runtime-selected V# pattern uses an 80-byte structured table. Dword 1 of each record is a
// selector, and selector*16 addresses the actual four-dword V# within the same allocation. Resolve
// only those referenced entries, then bind the union of their validated raw-buffer ranges.
//
// Descriptor heaps are populated lazily by the guest, so at any given dispatch the table may
// still hold unrelated bytes. A table that cannot be resolved therefore degrades to a null
// window (the shader-side descriptor checks drop the accesses) instead of failing the bind: on
// real hardware such a dispatch only dereferences entries its input data selects, and those are
// valid whenever the workload is real.
bool BuildBindlessWindow(const SrtRuntime& runtime, const DescriptorValue& table,
                         uint32_t first_use_pc, DescriptorValue* window, std::string* error) {
	(void)error;
	const auto table_base =
	    (static_cast<uint64_t>(table.dwords[1] & 0xffffu) << 32u | table.dwords[0]) & AddressMask;
	const auto table_stride  = (table.dwords[1] >> 16u) & 0x3fffu;
	const auto table_records = table.dwords[2];
	const auto table_bytes   = table_stride == 0
	                               ? static_cast<uint64_t>(table_records)
	                               : static_cast<uint64_t>(table_stride) * table_records;

	*window             = {};
	window->dword_count = 4;
	if (table_base == 0 || table_bytes < 16) {
		return true;
	}
	if (table_stride < 8 || table_bytes > MaxBindlessTableBytes ||
	    !GuestRangeReadable(runtime, table_base, table_bytes)) {
		WarnBindlessWindow(first_use_pc,
		                   fmt::format("table is invalid: base=0x{:012x} stride={} records={} "
		                               "bytes=0x{:x}",
		                               table_base, table_stride, table_records, table_bytes));
		return true;
	}

	std::vector<uint32_t> selectors;
	selectors.reserve(std::min<uint32_t>(table_records, 65536u));
	for (uint32_t record = 0; record < table_records; record++) {
		uint32_t   selector = 0;
		const auto selector_address =
		    table_base + static_cast<uint64_t>(record) * table_stride + sizeof(uint32_t);
		if (!ReadGuestDwords(runtime, selector_address, &selector, 1)) {
			WarnBindlessWindow(first_use_pc, fmt::format("selector {} is unreadable", record));
			return true;
		}
		const auto descriptor_offset = static_cast<uint64_t>(selector) * 16u;
		if (descriptor_offset <= table_bytes && table_bytes - descriptor_offset >= 16u) {
			selectors.push_back(selector);
		}
	}
	std::sort(selectors.begin(), selectors.end());
	selectors.erase(std::unique(selectors.begin(), selectors.end()), selectors.end());

	uint64_t union_begin = UINT64_MAX;
	uint64_t union_end   = 0;
	for (const auto selector: selectors) {
		uint32_t   entry[4]          = {};
		const auto descriptor_offset = static_cast<uint64_t>(selector) * 16u;
		if (!ReadGuestDwords(runtime, table_base + descriptor_offset, entry, 4)) {
			continue;
		}
		uint64_t begin = 0;
		uint64_t end   = 0;
		if (!DecodeBindlessRawBuffer(runtime, entry, &begin, &end)) {
			continue;
		}
		union_begin = std::min(union_begin, begin);
		union_end   = std::max(union_end, end);
	}
	if (union_begin >= union_end) {
		return true;
	}
	// Shifting the base for the device offset alignment is transparent to the shader because the
	// binding start and the emitted rebase delta move together.
	const auto aligned_begin = union_begin & ~(StorageBufferBindAlignment - 1u);
	if (aligned_begin != union_begin &&
	    !GuestRangeReadable(runtime, aligned_begin, union_begin - aligned_begin)) {
		WarnBindlessWindow(first_use_pc,
		                   fmt::format("aligned base 0x{:012x} below union base 0x{:012x} is "
		                               "not readable",
		                               aligned_begin, union_begin));
		*window             = {};
		window->dword_count = 4;
		return true;
	}
	union_begin            = aligned_begin;
	const auto union_bytes = union_end - union_begin;
	if (union_bytes > MaxBindlessWindowBytes || union_bytes > UINT32_MAX) {
		WarnBindlessWindow(first_use_pc,
		                   fmt::format("union spans 0x{:x} bytes (base=0x{:012x}); too large "
		                               "to bind",
		                               union_bytes, union_begin));
		*window             = {};
		window->dword_count = 4;
		return true;
	}
	window->dwords[0] = static_cast<uint32_t>(union_begin);
	window->dwords[1] = static_cast<uint32_t>(union_begin >> 32u) & 0xffffu;
	window->dwords[2] = static_cast<uint32_t>(union_bytes);
	window->dwords[3] = 0;
	return true;
}

} // namespace

uint32_t BindlessWindowCount(const Program& program) {
	uint32_t count = 0;
	for (const auto& buffer: program.info.buffers) {
		count += buffer.bindless ? 1u : 0u;
	}
	return count;
}

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot,
                              std::string* error) {
	if (!program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "shader resources were not tracked";
		}
		return false;
	}
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
	    snapshot.samplers.size() != program.info.samplers.size() ||
	    snapshot.addresses.size() != program.info.addresses.size()) {
		if (error != nullptr) {
			*error = "resource snapshot does not match dense shader topology";
		}
		return false;
	}
	if (snapshot.flattened_srt.size() !=
	    program.srt.reads.size() + program.info.buffers.size() + BindlessWindowCount(program)) {
		if (error != nullptr) {
			*error = "flattened SRT snapshot does not match the shader plan";
		}
		return false;
	}
	if (program.binding_layout_complete) {
		for (const auto reg: program.bindings.user_data_registers) {
			if (reg < program.user_data_base ||
			    reg - program.user_data_base >= snapshot.user_data.size()) {
				if (error != nullptr) {
					*error = fmt::format("runtime snapshot is missing user SGPR {}", reg);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto alias = program.info.buffers[i].image_alias;
		if (alias != BufferResource::NoImageAlias && alias >= program.info.images.size()) {
			if (error != nullptr) {
				*error = fmt::format("buffer resource {} has invalid image alias {}", i, alias);
			}
			return false;
		}
	}
	const auto CheckWidth = [&](const auto& values, uint32_t width, const char* kind) {
		for (uint32_t i = 0; i < values.size(); i++) {
			if (values[i].dword_count != width) {
				if (error != nullptr) {
					*error = fmt::format("{} descriptor {} has {} dwords", kind, i,
					                     values[i].dword_count);
				}
				return false;
			}
		}
		return true;
	};
	for (uint32_t i = 0; i < snapshot.addresses.size(); i++) {
		if (snapshot.addresses[i].binding_base > snapshot.addresses[i].guest_base) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} binds above its guest base", i);
			}
			return false;
		}
	}
	return CheckWidth(snapshot.buffers, 4, "buffer") && CheckWidth(snapshot.images, 8, "image") &&
	       CheckWidth(snapshot.samplers, 4, "sampler");
}

bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot,
                                    std::string* error) {
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
		return false;
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& buffer     = program.info.buffers[i];
		const auto& descriptor = snapshot.buffers[i];
		if (buffer.packed_stride != DescriptorPackedStride(descriptor) ||
		    buffer.descriptor_format != DescriptorBufferFormat(descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} no longer matches specialization", i);
			}
			return false;
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image      = program.info.images[i];
		const auto& descriptor = snapshot.images[i];
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor);
		if (dimension == Decoder::ImageDimension::Unknown || dimension != image.dimension) {
			if (error != nullptr) {
				*error =
				    fmt::format("image descriptor {} no longer matches specialized dimension", i);
			}
			return false;
		}
		if (image.kind == ResourceKind::Image || image.kind == ResourceKind::ImageUint ||
		    image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			const bool storage = image.kind == ResourceKind::StorageImage ||
			                     image.kind == ResourceKind::StorageImageUint;
			if (storage && image.storage_swizzle != DescriptorImageSwizzle(descriptor)) {
				if (error != nullptr) {
					*error = fmt::format("storage image descriptor {} changed swizzle", i);
				}
				return false;
			}
			const auto uint_descriptor =
			    Prospero::IsUintTextureFormat((descriptor.dwords[1] >> 20u) & 0x1ffu);
			const auto uint_program = image.kind == ResourceKind::ImageUint ||
			                          image.kind == ResourceKind::StorageImageUint;
			if (uint_descriptor != uint_program && !(image.atomic && uint_program)) {
				if (error != nullptr) {
					*error =
					    fmt::format("image descriptor {} no longer matches specialized format", i);
				}
				return false;
			}
			if (image.depth_compare && uint_program) {
				if (error != nullptr) {
					*error = fmt::format("integer image descriptor {} uses depth comparison", i);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		if (program.info.addresses[i].specialized_base !=
		    AddressSpecialization(program.info.addresses[i], snapshot.addresses[i])) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} no longer matches specialization", i);
			}
			return false;
		}
	}
	return true;
}

bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot* snapshot, std::string* error) {
	if (snapshot == nullptr || !program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = snapshot == nullptr ? "invalid resource snapshot"
			                             : "shader resources were not tracked";
		}
		return false;
	}

	std::vector<DescriptorSourceRequest> requests;
	requests.reserve(program.info.buffers.size() + program.info.images.size() +
	                 program.info.samplers.size() + program.info.addresses.size());
	// Tableless bindless buffers (V#s assembled from runtime pointers) have nothing to
	// evaluate; they window over the tracked-buffer union below.
	std::vector<uint32_t> buffer_slot(program.info.buffers.size(), UINT32_MAX);
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& buffer = program.info.buffers[i];
		if (buffer.bindless && buffer.table_source <= ScalarProvenance::Unknown) {
			continue;
		}
		buffer_slot[i] = static_cast<uint32_t>(requests.size());
		requests.push_back(
		    {buffer.bindless ? buffer.table_source : buffer.source, buffer.first_use_pc});
	}
	const auto buffer_request_count = static_cast<uint32_t>(requests.size());
	for (const auto& image: program.info.images) {
		requests.push_back({image.source, image.first_use_pc});
	}
	for (const auto& sampler: program.info.samplers) {
		requests.push_back({sampler.source, sampler.first_use_pc});
	}
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			requests.push_back({address.source, address.first_use_pc});
		}
	}

	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	if (!EvaluateRuntimeSources(program, requests, runtime, &values, &flattened_srt, error)) {
		return false;
	}

	ResourceSnapshot next;
	auto             cursor = values.begin();
	next.buffers.resize(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (buffer_slot[i] != UINT32_MAX) {
			next.buffers[i] = values[buffer_slot[i]];
		} else {
			next.buffers[i].dword_count = 4;
		}
	}
	cursor += buffer_request_count;

	// Fallback window for runtime pointer chases: the union of this dispatch's statically
	// tracked buffers. Also used when a descriptor-table scan cannot produce a usable window,
	// which otherwise pins polled memory at zero and can hang GPU-side spin loops.
	DescriptorValue union_window;
	union_window.dword_count = 4;
	{
		uint64_t union_begin = UINT64_MAX;
		uint64_t union_end   = 0;
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			if (program.info.buffers[i].bindless) {
				continue;
			}
			const auto& descriptor = next.buffers[i];
			const auto  base       = (static_cast<uint64_t>(descriptor.dwords[1] & 0xffffu) << 32u |
			                          descriptor.dwords[0]) &
			                         AddressMask;
			const auto  stride     = (descriptor.dwords[1] >> 16u) & 0x3fffu;
			const auto  records    = descriptor.dwords[2];
			const auto  bytes      = stride == 0 ? static_cast<uint64_t>(records)
			                                     : static_cast<uint64_t>(stride) * records;
			if (base == 0 || bytes == 0) {
				continue;
			}
			union_begin = std::min(union_begin, base);
			union_end   = std::max(union_end, base + bytes);
		}
		if (union_begin < union_end) {
			union_begin &= ~(StorageBufferBindAlignment - 1u);
			const auto union_bytes = union_end - union_begin;
			if (union_bytes <= MaxBindlessWindowBytes && union_bytes <= UINT32_MAX) {
				union_window.dwords[0] = static_cast<uint32_t>(union_begin);
				union_window.dwords[1] = static_cast<uint32_t>(union_begin >> 32u) & 0xffffu;
				union_window.dwords[2] = static_cast<uint32_t>(union_bytes);
			}
		}
	}

	std::vector<uint32_t> bindless_windows;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (!program.info.buffers[i].bindless) {
			continue;
		}
		DescriptorValue window;
		window.dword_count = 4;
		if (buffer_slot[i] != UINT32_MAX) {
			if (!BuildBindlessWindow(runtime, next.buffers[i], program.info.buffers[i].first_use_pc,
			                         &window, error)) {
				return false;
			}
		}
		if (window.dwords[0] == 0 && window.dwords[1] == 0) {
			window = union_window;
		}
		next.buffers[i] = window;
		bindless_windows.push_back(window.dwords[0]);
	}
	next.images.assign(cursor, cursor + program.info.images.size());
	cursor += program.info.images.size();
	for (auto& descriptor: next.images) {
		if (!ValidImageDescriptor(descriptor)) {
			descriptor.dwords.fill(0);
		}
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	cursor += program.info.samplers.size();
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			const auto value = *cursor++;
			auto       base  = (static_cast<uint64_t>(value.dwords[0]) |
			                    static_cast<uint64_t>(value.dwords[1]) << 32u) &
			                   AddressMask;
			if (address.kind == ResourceKind::ScalarBuffer) {
				base &= ~uint64_t {3};
			}
			const auto before = static_cast<uint64_t>(-static_cast<int64_t>(address.min_offset));
			const auto binding_base = address.kind == ResourceKind::Flat
			                              ? base & ~(FlatAddressWindowSize - 1u)
			                          : base >= before ? base - before
			                                           : 0;
			next.addresses.push_back({base, binding_base});
		} else if (runtime.flat_memory_base.has_value()) {
			next.addresses.push_back({*runtime.flat_memory_base, *runtime.flat_memory_base, 0});
		} else if (address.kind == ResourceKind::Flat) {
			if (error != nullptr) {
				*error = fmt::format("unbased FLAT address at pc 0x{:08x} requires runtime "
				                     "guest-address translation",
				                     address.first_use_pc);
			}
			return false;
		} else {
			// Runtime pointer chase (global/scratch address computed on the GPU). Window the
			// access over the union of this dispatch's tracked buffers: RAGE passes the
			// buffers such pointers target alongside the pointers themselves. Addresses
			// outside the window are dropped by the emitted bounds check.
			uint64_t union_begin = UINT64_MAX;
			uint64_t union_end   = 0;
			for (const auto& descriptor: next.buffers) {
				const auto base    = (static_cast<uint64_t>(descriptor.dwords[1] & 0xffffu) << 32u |
				                      descriptor.dwords[0]) &
				                     AddressMask;
				const auto stride  = (descriptor.dwords[1] >> 16u) & 0x3fffu;
				const auto records = descriptor.dwords[2];
				const auto bytes   = stride == 0 ? static_cast<uint64_t>(records)
				                                 : static_cast<uint64_t>(stride) * records;
				if (base == 0 || bytes == 0) {
					continue;
				}
				union_begin = std::min(union_begin, base);
				union_end   = std::max(union_end, base + bytes);
			}
			if (union_begin < union_end) {
				union_begin &= ~(StorageBufferBindAlignment - 1u);
			}
			const auto union_bytes = union_begin < union_end ? union_end - union_begin : 0;
			if (union_bytes == 0 || union_bytes > MaxBindlessWindowBytes) {
				WarnBindlessWindow(
				    address.first_use_pc,
				    fmt::format("unbased {} address window is unusable (span 0x{:x}); "
				                "accesses will be dropped",
				                address.kind == ResourceKind::Global ? "global" : "scratch",
				                union_bytes));
				next.addresses.push_back({0, 0, 0});
			} else {
				next.addresses.push_back({union_begin, union_begin, union_bytes});
			}
		}
	}
	next.flattened_srt = std::move(flattened_srt);
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		// Bindless buffers rebase through their window base instead; their binding is already
		// aligned, so the generic remainder stays zero.
		uint32_t remainder = 0;
		if (!program.info.buffers[i].bindless) {
			const auto base = (static_cast<uint64_t>(next.buffers[i].dwords[1] & 0xffffu) << 32u |
			                   next.buffers[i].dwords[0]) &
			                  AddressMask;
			remainder       = static_cast<uint32_t>(base & (StorageBufferBindAlignment - 1u));
		}
		next.flattened_srt.push_back(remainder);
	}
	next.flattened_srt.insert(next.flattened_srt.end(), bindless_windows.begin(),
	                          bindless_windows.end());
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	if (!ValidateResourceSnapshot(program, next, error)) {
		return false;
	}
	*snapshot = std::move(next);
	return true;
}

bool SpecializeResources(Program* program, const ResourceSnapshot& snapshot, std::string* error) {
	if (program == nullptr || !program->resource_tracking_complete ||
	    program->shader_info_complete || program->binding_layout_complete) {
		if (error != nullptr) {
			*error = program == nullptr ? "invalid resource specialization program"
			         : !program->resource_tracking_complete ? "shader resources were not tracked"
			                                                : "resource specialization is too late";
		}
		return false;
	}
	if (!ValidateResourceSnapshot(*program, snapshot, error)) {
		return false;
	}

	auto next = program->info;
	for (uint32_t i = 0; i < next.buffers.size(); i++) {
		next.buffers[i].packed_stride     = DescriptorPackedStride(snapshot.buffers[i]);
		next.buffers[i].descriptor_format = DescriptorBufferFormat(snapshot.buffers[i]);
	}
	for (uint32_t i = 0; i < next.addresses.size(); i++) {
		next.addresses[i].specialized_base =
		    AddressSpecialization(next.addresses[i], snapshot.addresses[i]);
	}
	for (uint32_t i = 0; i < next.images.size(); i++) {
		const auto& descriptor = snapshot.images[i];
		auto&       image      = next.images[i];
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			if (error != nullptr) {
				*error = fmt::format(
				    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
				    "{:08x},{:08x},{:08x},{:08x}",
				    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0],
				    descriptor.dwords[1], descriptor.dwords[2], descriptor.dwords[3],
				    descriptor.dwords[4], descriptor.dwords[5], descriptor.dwords[6],
				    descriptor.dwords[7]);
			}
			return false;
		}
		image.dimension = descriptor_dimension;
		if (image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			image.storage_swizzle = DescriptorImageSwizzle(descriptor);
		}
		if (Prospero::IsUintTextureFormat((descriptor.dwords[1] >> 20u) & 0x1ffu)) {
			switch (image.kind) {
				case ResourceKind::Image: image.kind = ResourceKind::ImageUint; break;
				case ResourceKind::StorageImage: image.kind = ResourceKind::StorageImageUint; break;
				default: break;
			}
		}
	}
	struct ImagePatch {
		Instruction*            inst;
		ResourceKind            kind;
		Decoder::ImageDimension dimension;
	};
	std::vector<ImagePatch> patches;
	for (auto& block: program->blocks) {
		for (auto& inst: block.instructions) {
			if (inst.memory.kind != ResourceKind::Image &&
			    inst.memory.kind != ResourceKind::ImageUint &&
			    inst.memory.kind != ResourceKind::StorageImage &&
			    inst.memory.kind != ResourceKind::StorageImageUint) {
				continue;
			}
			if (inst.memory.resource >= next.images.size()) {
				if (error != nullptr) {
					*error = fmt::format("image instruction at pc 0x{:08x} has invalid resource {}",
					                     inst.pc, inst.memory.resource);
				}
				return false;
			}
			const auto& image = next.images[inst.memory.resource];
			patches.push_back({&inst, image.kind, image.dimension});
		}
	}
	program->info = std::move(next);
	for (const auto& patch: patches) {
		patch.inst->memory.kind            = patch.kind;
		patch.inst->memory.image_dimension = patch.dimension;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
