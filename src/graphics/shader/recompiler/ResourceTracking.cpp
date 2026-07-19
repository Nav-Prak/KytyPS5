#include "graphics/shader/recompiler/ResourceTracking.h"

#include "graphics/shader/recompiler/ScalarProvenance.h"

#include <algorithm>
#include <fmt/format.h>
#include <map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

bool IsAtomic(Opcode op) {
	switch (op) {
		case Opcode::AtomicSwapU32:
		case Opcode::AtomicSwapU64:
		case Opcode::AtomicAddU32:
		case Opcode::AtomicSubU32:
		case Opcode::AtomicSMinI32:
		case Opcode::AtomicUMinU32:
		case Opcode::AtomicSMaxI32:
		case Opcode::AtomicUMaxU32:
		case Opcode::AtomicAndU32:
		case Opcode::AtomicOrU32:
		case Opcode::AtomicXorU32:
		case Opcode::AtomicFMinF32:
		case Opcode::AtomicFMaxF32: return true;
		default: return false;
	}
}

bool IsWrite(Opcode op) {
	switch (op) {
		case Opcode::BufferStoreByte:
		case Opcode::BufferStoreShort:
		case Opcode::BufferStoreDword:
		case Opcode::ImageStore: return true;
		default: return IsAtomic(op);
	}
}

bool NeedsSampler(Opcode op) {
	return op == Opcode::ImageSample || op == Opcode::ImageGather4 || op == Opcode::ImageGetLod;
}

bool IsDepthCompare(const Instruction& inst) {
	return (inst.memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0;
}

ImageMipMode MipMode(const Instruction& inst) {
	const bool storage = inst.memory.kind == ResourceKind::StorageImage ||
	                     inst.memory.kind == ResourceKind::StorageImageUint;
	return storage && inst.memory.image_has_mip ? ImageMipMode::DynamicStorage : ImageMipMode::None;
}

bool IsBuffer(const Instruction& inst) {
	return inst.memory.kind == ResourceKind::Buffer ||
	       (inst.memory.kind == ResourceKind::ScalarBuffer && inst.op == Opcode::SBufferLoadDword);
}

bool IsImage(const Instruction& inst) {
	return inst.memory.kind == ResourceKind::Image || inst.memory.kind == ResourceKind::ImageUint ||
	       inst.memory.kind == ResourceKind::StorageImage ||
	       inst.memory.kind == ResourceKind::StorageImageUint;
}

bool IsAddress(const Instruction& inst) {
	return inst.op == Opcode::SLoadDword || inst.memory.kind == ResourceKind::Flat ||
	       inst.memory.kind == ResourceKind::Global || inst.memory.kind == ResourceKind::Scratch;
}

std::string InstructionDiagnostic(const Instruction& inst) {
	return fmt::format("0x{:08x}: op={} {}", inst.pc, static_cast<uint32_t>(inst.op),
	                   InstructionToString(inst));
}

uint32_t ByteExtent(const Instruction& inst) {
	const auto bytes = std::max((inst.memory.data_bits + 7u) / 8u, 1u);
	const auto count = std::max(inst.memory.data_dwords, 1u);
	const auto end =
	    static_cast<uint64_t>(inst.memory.offset) + static_cast<uint64_t>(bytes) * count;
	return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

bool ContainsUnknown(const ScalarProvenance& provenance, uint32_t id, std::vector<uint8_t>* visited,
                     std::vector<uint32_t>* path) {
	path->push_back(id);
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return true;
	}
	if ((*visited)[id] != 0) {
		path->pop_back();
		return false;
	}
	(*visited)[id]    = 1;
	const auto& value = provenance.values[id];
	if (value.op == ScalarValueOp::Phi) {
		for (const auto arg: value.phi_args) {
			if (ContainsUnknown(provenance, arg, visited, path)) {
				return true;
			}
		}
		path->pop_back();
		return false;
	}
	const auto args = ScalarValueArgCount(value.op);
	for (uint32_t i = 0; i < args; i++) {
		if (ContainsUnknown(provenance, value.args[i], visited, path)) {
			return true;
		}
	}
	path->pop_back();
	return false;
}

bool IsLoopInvariantValue(const ScalarProvenance& provenance, uint32_t id,
                          std::vector<uint8_t>* visiting) {
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return false;
	}
	if ((*visiting)[id] != 0) {
		return true;
	}
	(*visiting)[id]   = 1;
	const auto& value = provenance.values[id];
	if (value.op == ScalarValueOp::Phi) {
		uint32_t invariant = ScalarProvenance::Undefined;
		for (const auto arg: value.phi_args) {
			if (arg == id) {
				continue;
			}
			if (invariant == ScalarProvenance::Undefined) {
				invariant = arg;
			} else if (arg != invariant) {
				return false;
			}
		}
		return invariant != ScalarProvenance::Undefined &&
		       IsLoopInvariantValue(provenance, invariant, visiting);
	}
	for (uint32_t i = 0; i < ScalarValueArgCount(value.op); i++) {
		if (!IsLoopInvariantValue(provenance, value.args[i], visiting)) {
			return false;
		}
	}
	return true;
}

bool IsLoopInvariantDescriptor(const ScalarProvenance& provenance,
                               const DescriptorValue&  descriptor) {
	std::vector<uint8_t> visiting(provenance.values.size());
	for (uint32_t i = 0; i < descriptor.dword_count; i++) {
		if (!IsLoopInvariantValue(provenance, descriptor.dwords[i], &visiting)) {
			return false;
		}
	}
	return true;
}

class Tracker {
public:
	explicit Tracker(Program& program): m_program(program) {}

	bool Run(std::string* error) {
		if (m_program.resource_tracking_complete) {
			return Fail(0, error, "resources already tracked");
		}
		if (!m_program.srt_plan_complete) {
			return Fail(0, error, "SRT plan is not ready");
		}
		if (!m_program.srt_patching_complete) {
			return Fail(0, error, "SRT reads were not patched");
		}
		for (auto& block: m_program.blocks) {
			for (auto& inst: block.instructions) {
				if (!Collect(&inst, error)) {
					return false;
				}
			}
		}
		LinkImageAliases();
		m_program.info = std::move(m_info);
		for (const auto& patch: m_patches) {
			patch.inst->memory.resource        = patch.resource;
			patch.inst->memory.sampler         = patch.sampler;
			patch.inst->memory.resource_source = ScalarProvenance::Undefined;
			patch.inst->memory.sampler_source  = ScalarProvenance::Undefined;
			patch.inst->memory.bindless_sgprs  = patch.bindless_sgprs;
		}
		m_program.resource_tracking_complete = true;
		return true;
	}

private:
	struct Patch {
		Instruction* inst           = nullptr;
		uint32_t     resource       = 0;
		uint32_t     sampler        = 0;
		uint32_t     bindless_sgprs = UINT32_MAX;
	};

	bool Fail(uint32_t pc, std::string* error, const std::string& reason) const {
		if (error != nullptr) {
			*error = fmt::format("shader resource tracking: hash=0x{:016x} stage={} pc=0x{:08x} {}",
			                     m_program.shader_hash, StageName(m_program.stage), pc, reason);
		}
		return false;
	}

	std::string FormatInstructionContext(const std::vector<uint32_t>& path, uint32_t use_pc) const {
		std::vector<uint32_t> targets {use_pc};
		for (const auto id: path) {
			if (id < m_program.provenance.values.size()) {
				const auto pc = m_program.provenance.values[id].pc;
				if (pc != 0 && std::find(targets.begin(), targets.end(), pc) == targets.end()) {
					targets.push_back(pc);
				}
			}
		}

		std::string context;
		for (const auto target: targets) {
			for (const auto& block: m_program.blocks) {
				const auto found =
				    std::find_if(block.instructions.begin(), block.instructions.end(),
				                 [target](const Instruction& inst) { return inst.pc == target; });
				if (found == block.instructions.end()) {
					continue;
				}
				const auto index = static_cast<size_t>(found - block.instructions.begin());
				const auto begin = index > 8 ? index - 8 : 0;
				const auto end   = std::min(index + 3, block.instructions.size());
				context += fmt::format(" context around 0x{:08x}:", target);
				for (auto i = begin; i < end; i++) {
					context += fmt::format("\n  {}", InstructionDiagnostic(block.instructions[i]));
				}
				break;
			}
		}
		return context;
	}

	std::string FormatUniformVectorProducers(uint32_t use_pc) const {
		struct UniformRead {
			Register source;
			uint32_t pc = 0;
		};
		std::vector<UniformRead> reads;
		for (const auto& block: m_program.blocks) {
			const auto use =
			    std::find_if(block.instructions.begin(), block.instructions.end(),
			                 [use_pc](const Instruction& inst) { return inst.pc == use_pc; });
			if (use == block.instructions.end()) {
				continue;
			}
			const auto begin = use - std::min<size_t>(16, use - block.instructions.begin());
			for (auto current = begin; current != use; ++current) {
				if (current->op != Opcode::ReadFirstLaneU32 || current->src_count == 0 ||
				    current->src[0].kind != OperandKind::Register ||
				    current->src[0].reg.file != RegisterFile::Vector) {
					continue;
				}
				const UniformRead read {current->src[0].reg, current->pc};
				if (std::find_if(reads.begin(), reads.end(), [&read](const UniformRead& existing) {
					    return existing.source == read.source && existing.pc == read.pc;
				    }) == reads.end()) {
					reads.push_back(read);
				}
			}
			break;
		}

		std::string detail;
		for (const auto& read: reads) {
			detail += fmt::format(" uniform vector source {} read at 0x{:08x}; writers:",
			                      RegisterToString(read.source), read.pc);
			uint32_t writers = 0;
			for (const auto& block: m_program.blocks) {
				for (size_t index = 0; index < block.instructions.size(); index++) {
					const auto& inst = block.instructions[index];
					const bool  writes =
					    (inst.dst.kind == OperandKind::Register && inst.dst.reg == read.source) ||
					    (inst.dst2.kind == OperandKind::Register && inst.dst2.reg == read.source);
					if (!writes) {
						continue;
					}
					if (writers++ >= 16) {
						detail += "\n  additional writers omitted";
						break;
					}
					const auto first = index > 4 ? index - 4 : 0;
					const auto last  = std::min(index + 3, block.instructions.size());
					detail += fmt::format("\n  writer context around 0x{:08x}:", inst.pc);
					for (auto i = first; i < last; i++) {
						detail +=
						    fmt::format("\n    {}", InstructionDiagnostic(block.instructions[i]));
					}
					std::vector<Register> dependencies;
					for (uint32_t source_index = 0; source_index < inst.src_count; source_index++) {
						const auto& source = inst.src[source_index];
						if (source.kind == OperandKind::Register &&
						    source.reg.file == RegisterFile::Vector && source.reg != read.source &&
						    std::find(dependencies.begin(), dependencies.end(), source.reg) ==
						        dependencies.end()) {
							dependencies.push_back(source.reg);
						}
					}
					for (const auto dependency: dependencies) {
						detail += FormatVectorDependency(dependency, 2);
					}
				}
				if (writers > 16) {
					break;
				}
			}
			if (writers == 0) {
				detail += " none (stage input or undefined)";
			}
		}
		return detail;
	}

	std::string FormatVectorDependency(Register source, uint32_t depth) const {
		std::string detail =
		    fmt::format("\n    vector dependency {} writers:", RegisterToString(source));
		uint32_t writers = 0;
		for (const auto& block: m_program.blocks) {
			for (size_t index = 0; index < block.instructions.size(); index++) {
				const auto& inst = block.instructions[index];
				const bool  writes =
				    (inst.dst.kind == OperandKind::Register && inst.dst.reg == source) ||
				    (inst.dst2.kind == OperandKind::Register && inst.dst2.reg == source);
				if (!writes) {
					continue;
				}
				if (writers++ >= 12) {
					detail += "\n      additional writers omitted";
					return detail;
				}
				detail += fmt::format("\n      {}", InstructionDiagnostic(inst));
				if (depth == 0) {
					continue;
				}
				for (uint32_t source_index = 0; source_index < inst.src_count; source_index++) {
					const auto& dependency = inst.src[source_index];
					if (dependency.kind == OperandKind::Register &&
					    dependency.reg.file == RegisterFile::Vector && dependency.reg != source) {
						detail += FormatVectorDependency(dependency.reg, depth - 1);
					}
				}
			}
		}
		if (writers == 0) {
			detail += " none (stage input or undefined)";
		}
		return detail;
	}

	std::string FormatValueArguments(uint32_t id) const {
		if (id >= m_program.provenance.values.size()) {
			return {};
		}
		const auto& value = m_program.provenance.values[id];
		const auto  count = ScalarValueArgCount(value.op);
		std::string detail;
		for (uint32_t i = 0; i < count; i++) {
			const auto arg = value.args[i];
			const auto op  = arg < m_program.provenance.values.size()
			                     ? static_cast<uint32_t>(m_program.provenance.values[arg].op)
			                     : UINT32_MAX;
			detail += fmt::format(" arg{}={}:{}({})", i, arg, op,
			                      ScalarValueToString(m_program.provenance, arg));
		}
		return detail;
	}

	// Recognizes a V# whose four dwords were fetched by one runtime SBufferLoadDwordX4 from a
	// CPU-resolvable descriptor-table V#. Returns the interned table descriptor source, or
	// Undefined when the source does not match the pattern.
	uint32_t ClassifyBindlessSource(uint32_t source) {
		if (const auto cached = m_bindless_sources.find(source);
		    cached != m_bindless_sources.end()) {
			return cached->second;
		}
		const auto classify = [&]() -> uint32_t {
			const auto* descriptor = GetDescriptorSource(m_program, source);
			if (descriptor == nullptr || descriptor->dword_count != 4) {
				return ScalarProvenance::Undefined;
			}
			const auto& values = m_program.provenance.values;
			const auto  first  = descriptor->dwords[0];
			if (first >= values.size() || values[first].op != ScalarValueOp::ReadConstBuffer) {
				return ScalarProvenance::Undefined;
			}
			const auto& base_node = values[first];
			for (uint32_t i = 1; i < 4; i++) {
				const auto id = descriptor->dwords[i];
				if (id >= values.size()) {
					return ScalarProvenance::Undefined;
				}
				const auto& node = values[id];
				if (node.op != ScalarValueOp::ReadConstBuffer || node.pc != base_node.pc ||
				    node.imm != base_node.imm + i * 4u ||
				    !std::equal(node.args.begin(), node.args.begin() + 5, base_node.args.begin())) {
					return ScalarProvenance::Undefined;
				}
			}
			DescriptorValue table;
			table.dword_count = 4;
			for (uint32_t i = 0; i < 4; i++) {
				table.dwords[i] = base_node.args[i];
				if (!ScalarValueResolved(m_program, table.dwords[i])) {
					return ScalarProvenance::Undefined;
				}
			}
			return InternDescriptorSource(&m_program, table);
		};
		const auto table_source = classify();
		m_bindless_sources.emplace(source, table_source);
		return table_source;
	}

	bool SupportsBindlessAccess(const Instruction& inst) const {
		return inst.memory.kind == ResourceKind::Buffer && !inst.memory.formatted &&
		       !inst.memory.typed;
	}

	bool ValidateSource(uint32_t source, uint32_t dwords, uint32_t pc, std::string* error) const {
		const auto* descriptor = GetDescriptorSource(m_program, source);
		if (descriptor == nullptr || descriptor->dword_count != dwords) {
			return Fail(pc, error,
			            fmt::format("descriptor source {} is missing or has wrong width", source));
		}
		std::vector<uint8_t> visited(m_program.provenance.values.size());
		for (uint32_t i = 0; i < descriptor->dword_count; i++) {
			std::vector<uint32_t> path;
			if (ContainsUnknown(m_program.provenance, descriptor->dwords[i], &visited, &path)) {
				const auto  value = descriptor->dwords[i];
				std::string chain;
				for (const auto id: path) {
					const auto op = id < m_program.provenance.values.size()
					                    ? static_cast<uint32_t>(m_program.provenance.values[id].op)
					                    : UINT32_MAX;
					const auto node_pc = id < m_program.provenance.values.size()
					                         ? m_program.provenance.values[id].pc
					                         : 0;
					chain +=
					    fmt::format("{}{}:{}@0x{:08x}({})", chain.empty() ? "" : " -> ", id, op,
					                node_pc, ScalarValueToString(m_program.provenance, id));
				}
				return Fail(pc, error,
				            fmt::format("descriptor source {} dword {} contains an unknown value "
				                        "{} ({}){} path {}{}",
				                        source, i, value,
				                        ScalarValueToString(m_program.provenance, value),
				                        FormatValueArguments(value), chain,
				                        FormatInstructionContext(path, pc) +
				                            FormatUniformVectorProducers(pc)));
			}
		}
		const auto dynamic =
		    std::find(m_program.srt.dynamic_sources.begin(), m_program.srt.dynamic_sources.end(),
		              source) != m_program.srt.dynamic_sources.end();
		if (dynamic && !IsLoopInvariantDescriptor(m_program.provenance, *descriptor)) {
			std::string detail;
			for (uint32_t i = 0; i < descriptor->dword_count; i++) {
				const auto id = descriptor->dwords[i];
				detail +=
				    fmt::format(" d{}={}({})", i, id,
				                id < m_program.provenance.values.size()
				                    ? static_cast<uint32_t>(m_program.provenance.values[id].op)
				                    : UINT32_MAX);
				if (id < m_program.provenance.values.size()) {
					for (const auto arg: m_program.provenance.values[id].phi_args) {
						detail += fmt::format(
						    "/{}({})", arg,
						    arg < m_program.provenance.values.size()
						        ? static_cast<uint32_t>(m_program.provenance.values[arg].op)
						        : UINT32_MAX);
					}
				}
			}
			return Fail(pc, error,
			            fmt::format("descriptor source {} requires unsupported GPU selection{}",
			                        source, detail));
		}
		if (!DescriptorSourceResolved(m_program, source) && !dynamic) {
			return Fail(pc, error, fmt::format("descriptor source {} is unresolved", source));
		}
		return true;
	}

	uint32_t AddBuffer(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.buffers.size(); i++) {
			if (m_info.buffers[i].source == inst.memory.resource_source) {
				Merge(&m_info.buffers[i], inst);
				return i;
			}
		}
		if (m_info.buffers.size() >= ShaderInfo::MaxBuffers) {
			return UINT32_MAX;
		}
		BufferResource resource;
		resource.source       = inst.memory.resource_source;
		resource.first_use_pc = inst.pc;
		Merge(&resource, inst);
		m_info.buffers.push_back(resource);
		return static_cast<uint32_t>(m_info.buffers.size() - 1);
	}

	void Merge(BufferResource* resource, const Instruction& inst) const {
		resource->first_use_pc    = std::min(resource->first_use_pc, inst.pc);
		resource->max_byte_extent = std::max(resource->max_byte_extent, ByteExtent(inst));
		resource->read            = resource->read || !IsWrite(inst.op) || IsAtomic(inst.op);
		resource->written         = resource->written || IsWrite(inst.op);
		resource->atomic          = resource->atomic || IsAtomic(inst.op);
		resource->formatted       = resource->formatted || inst.memory.formatted;
		resource->scalar = resource->scalar || inst.memory.kind == ResourceKind::ScalarBuffer;
	}

	void LinkImageAliases() {
		for (auto& buffer: m_info.buffers) {
			const auto* buffer_source = GetDescriptorSource(m_program, buffer.source);
			if (buffer_source == nullptr || buffer_source->dword_count != 4) {
				continue;
			}
			for (uint32_t i = 0; i < m_info.images.size(); i++) {
				const auto* image_source = GetDescriptorSource(m_program, m_info.images[i].source);
				if (image_source != nullptr && image_source->dword_count == 8 &&
				    std::equal(buffer_source->dwords.begin(), buffer_source->dwords.begin() + 4,
				               image_source->dwords.begin())) {
					buffer.image_alias = i;
					break;
				}
			}
		}
	}

	uint32_t AddImage(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.images.size(); i++) {
			auto& image = m_info.images[i];
			if (image.source == inst.memory.resource_source && image.kind == inst.memory.kind &&
			    image.dimension == inst.memory.image_dimension && image.mip_mode == MipMode(inst) &&
			    image.depth_compare == IsDepthCompare(inst)) {
				Merge(&image, inst);
				return i;
			}
		}
		if (m_info.images.size() >= ShaderInfo::MaxImages) {
			return UINT32_MAX;
		}
		ImageResource image;
		image.source        = inst.memory.resource_source;
		image.first_use_pc  = inst.pc;
		image.kind          = inst.memory.kind;
		image.dimension     = inst.memory.image_dimension;
		image.mip_mode      = MipMode(inst);
		image.depth_compare = IsDepthCompare(inst);
		Merge(&image, inst);
		m_info.images.push_back(image);
		return static_cast<uint32_t>(m_info.images.size() - 1);
	}

	uint32_t AddAddress(const Instruction& inst) {
		auto immediate = static_cast<int32_t>(inst.memory.offset);
		if (inst.memory.kind == ResourceKind::ScalarBuffer) {
			immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
		}
		const auto min_offset =
		    inst.memory.resource_source == ScalarProvenance::Unknown ? 0 : std::min(immediate, 0);
		for (uint32_t i = 0; i < m_info.addresses.size(); i++) {
			auto& address = m_info.addresses[i];
			if (address.source == inst.memory.resource_source && address.kind == inst.memory.kind) {
				address.first_use_pc = std::min(address.first_use_pc, inst.pc);
				address.min_offset   = std::min(address.min_offset, min_offset);
				address.read         = address.read || !IsWrite(inst.op) || IsAtomic(inst.op);
				address.written      = address.written || IsWrite(inst.op);
				address.atomic       = address.atomic || IsAtomic(inst.op);
				return i;
			}
		}
		if (m_info.addresses.size() >= ShaderInfo::MaxAddresses) {
			return UINT32_MAX;
		}
		AddressResource address {inst.memory.resource_source, inst.pc, inst.memory.kind,
		                         min_offset};
		address.read    = !IsWrite(inst.op) || IsAtomic(inst.op);
		address.written = IsWrite(inst.op);
		address.atomic  = IsAtomic(inst.op);
		m_info.addresses.push_back(address);
		return static_cast<uint32_t>(m_info.addresses.size() - 1);
	}

	void Merge(ImageResource* resource, const Instruction& inst) const {
		resource->first_use_pc = std::min(resource->first_use_pc, inst.pc);
		resource->read         = resource->read || !IsWrite(inst.op) || IsAtomic(inst.op);
		resource->written      = resource->written || IsWrite(inst.op);
		resource->atomic       = resource->atomic || IsAtomic(inst.op);
	}

	uint32_t AddSampler(const Instruction& inst) {
		for (uint32_t i = 0; i < m_info.samplers.size(); i++) {
			if (m_info.samplers[i].source == inst.memory.sampler_source) {
				m_info.samplers[i].first_use_pc =
				    std::min(m_info.samplers[i].first_use_pc, inst.pc);
				return i;
			}
		}
		if (m_info.samplers.size() >= ShaderInfo::MaxSamplers) {
			return UINT32_MAX;
		}
		m_info.samplers.push_back({inst.memory.sampler_source, inst.pc});
		return static_cast<uint32_t>(m_info.samplers.size() - 1);
	}

	bool AddSampledPair(uint32_t image, uint32_t sampler, uint32_t pc, std::string* error) {
		for (auto& pair: m_info.sampled_pairs) {
			if (pair.image == image && pair.sampler == sampler) {
				pair.first_use_pc = std::min(pair.first_use_pc, pc);
				return true;
			}
		}
		if (m_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
			return Fail(pc, error, "sampled image/sampler pair limit exceeded");
		}
		m_info.sampled_pairs.push_back({image, sampler, pc});
		return true;
	}

	bool Collect(Instruction* inst, std::string* error) {
		if (IsAddress(*inst)) {
			// Addresses whose provenance cannot be resolved are pointer chases computed on the
			// GPU; demote them to the unbased runtime-address path, where the emitted code reads
			// the live registers and rebases onto the bound window. Scalar loads remember their
			// base SGPR pair because patching overwrites the resource field.
			bool demoted_scalar = false;
			if (inst->memory.resource_source != ScalarProvenance::Unknown &&
			    (inst->op == Opcode::SLoadDword || inst->memory.kind == ResourceKind::Global ||
			     inst->memory.kind == ResourceKind::Scratch)) {
				std::string validate_error;
				if (!ValidateSource(inst->memory.resource_source, 2, inst->pc, &validate_error)) {
					// Keep the original source reachable for user-data collection: the base
					// registers are not IR operands, so without this their user-SGPR roots
					// would vanish from the push constants.
					if (std::find(m_program.srt.dynamic_sources.begin(),
					              m_program.srt.dynamic_sources.end(),
					              inst->memory.resource_source) ==
					    m_program.srt.dynamic_sources.end()) {
						m_program.srt.dynamic_sources.push_back(inst->memory.resource_source);
					}
					inst->memory.resource_source = ScalarProvenance::Unknown;
					if (inst->op == Opcode::SLoadDword) {
						demoted_scalar              = true;
						inst->memory.bindless_sgprs = inst->memory.resource;
					}
				}
			}
			const bool unbased = inst->memory.resource_source == ScalarProvenance::Unknown;
			if ((!unbased && !ValidateSource(inst->memory.resource_source, 2, inst->pc, error)) ||
			    (unbased && !demoted_scalar && inst->memory.kind != ResourceKind::Flat &&
			     inst->memory.kind != ResourceKind::Global &&
			     inst->memory.kind != ResourceKind::Scratch)) {
				return unbased ? Fail(inst->pc, error, "scalar memory base is unresolved") : false;
			}
			const auto resource = AddAddress(*inst);
			if (resource == UINT32_MAX) {
				return Fail(inst->pc, error, "address resource limit exceeded");
			}
			m_patches.push_back({inst, resource, 0, inst->memory.bindless_sgprs});
			return true;
		}
		if (!IsBuffer(*inst) && !IsImage(*inst)) {
			return true;
		}
		std::string validate_error;
		if (!ValidateSource(inst->memory.resource_source, IsBuffer(*inst) ? 4u : 8u, inst->pc,
		                    &validate_error)) {
			if (!IsBuffer(*inst) || !SupportsBindlessAccess(*inst)) {
				if (error != nullptr) {
					*error = std::move(validate_error);
				}
				return false;
			}
			// Table pattern when recognizable; otherwise the V# was assembled in registers from
			// a runtime pointer. Either way the live quad holds the descriptor at the use, so
			// the bindless emission path applies; a tableless buffer windows over the
			// dispatch's tracked-buffer union at materialization.
			const auto table_source = ClassifyBindlessSource(inst->memory.resource_source);
			Patch      patch;
			patch.inst           = inst;
			patch.bindless_sgprs = inst->memory.resource * 4u;
			patch.resource       = AddBuffer(*inst);
			if (patch.resource == UINT32_MAX) {
				return Fail(inst->pc, error, "buffer resource limit exceeded");
			}
			auto& buffer = m_info.buffers[patch.resource];
			if (buffer.bindless && buffer.table_source != table_source) {
				return Fail(inst->pc, error, "bindless buffer table source conflict");
			}
			buffer.bindless     = true;
			buffer.table_source = table_source;
			m_patches.push_back(patch);
			return true;
		}
		Patch patch;
		patch.inst     = inst;
		patch.resource = IsBuffer(*inst) ? AddBuffer(*inst) : AddImage(*inst);
		if (patch.resource == UINT32_MAX) {
			return Fail(inst->pc, error,
			            IsBuffer(*inst) ? "buffer resource limit exceeded"
			                            : "image resource limit exceeded");
		}
		if (NeedsSampler(inst->op)) {
			if (!ValidateSource(inst->memory.sampler_source, 4, inst->pc, error)) {
				return false;
			}
			patch.sampler = AddSampler(*inst);
			if (patch.sampler == UINT32_MAX) {
				return Fail(inst->pc, error, "sampler resource limit exceeded");
			}
			if (!AddSampledPair(patch.resource, patch.sampler, inst->pc, error)) {
				return false;
			}
		}
		m_patches.push_back(patch);
		return true;
	}

	Program&                     m_program;
	ShaderInfo                   m_info;
	std::vector<Patch>           m_patches;
	std::map<uint32_t, uint32_t> m_bindless_sources;
};

} // namespace

bool TrackResources(Program* program, std::string* error) {
	if (program == nullptr) {
		if (error != nullptr) {
			*error = "shader resource tracking: hash=0x0000000000000000 stage=unknown "
			         "pc=0x00000000 invalid program";
		}
		return false;
	}
	return Tracker(*program).Run(error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
