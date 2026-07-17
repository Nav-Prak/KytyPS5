#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderInternal.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/shaderSubgroup.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>

// IWYU pragma: no_forward_declare VkImageView_T

namespace Libs::Graphics {

static std::atomic<uint32_t> g_draw_state_log_count       = 0;
static std::atomic<uint32_t> g_draw_input_log_count       = 0;
static std::atomic<uint32_t> g_mrt_state_log_count        = 0;
static std::atomic<uint32_t> g_shader_stage_log_count     = 0;
static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static bool ResolveColorTargets(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                                uint32_t render_target_slice_offset);

static bool ReadLogIndices(uint32_t index_type, const void* address, std::span<uint32_t> indices) {
	if (address == nullptr || indices.empty()) {
		return false;
	}
	const auto guest_address = reinterpret_cast<uint64_t>(address);
	switch (static_cast<Prospero::IndexType>(index_type)) {
		case Prospero::IndexType::kIndex8: {
			std::vector<uint8_t> raw(indices.size());
			if (!Libs::LibKernel::Memory::TryReadBacking(guest_address, raw.data(), raw.size())) {
				return false;
			}
			std::copy(raw.begin(), raw.end(), indices.begin());
			return true;
		}
		case Prospero::IndexType::kIndex16: {
			std::vector<uint16_t> raw(indices.size());
			if (!Libs::LibKernel::Memory::TryReadBacking(guest_address, raw.data(),
			                                             raw.size() * sizeof(raw[0]))) {
				return false;
			}
			std::copy(raw.begin(), raw.end(), indices.begin());
			return true;
		}
		case Prospero::IndexType::kIndex32:
			return Libs::LibKernel::Memory::TryReadBacking(guest_address, indices.data(),
			                                               indices.size_bytes());
		default: return false;
	}
}

static const char* RenderColorTypeName(RenderColorType type) {
	switch (type) {
		case RenderColorType::NoColorOutput: return "NoColorOutput";
		case RenderColorType::DisplayBuffer: return "DisplayBuffer";
		case RenderColorType::RenderTexture: return "RenderTexture";
		default: return "Unknown";
	}
}

static bool IsDualSourceBlendFactor(uint32_t factor) {
	return factor >= 0x0fu && factor <= 0x12u;
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const HW::Context* ctx,
                               const HW::UserConfig* ucfg, uint32_t index_count, uint32_t flags) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color.type), color.base_addr, color.buffer_size,
	    color.vulkan_buffer != nullptr ? "yes" : "no", string_VkFormat(depth.format),
	    depth.vulkan_buffer != nullptr ? "yes" : "no", depth.vaddr_num,
	    ctx != nullptr ? ctx->GetRenderTargetMask() : 0, ucfg != nullptr ? ucfg->GetPrimType() : 0,
	    index_count, flags);
}

static void LogMrtState(const char* draw_name, const HW::Context* ctx,
                        const ShaderPixelInputInfo& ps_input_info) {
	EXIT_IF(ctx == nullptr);

	const auto& sh_regs        = ctx->GetShaderRegisters();
	const auto  rt_mask        = ctx->GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx->GetBlendControl(0);

	bool interesting = rt_mask != 0x0f || (cb_shader_mask & ~0x0fu) != 0 ||
	                   IsDualSourceBlendFactor(bc0.color_srcblend) ||
	                   IsDualSourceBlendFactor(bc0.color_destblend) ||
	                   (bc0.separate_alpha_blend && (IsDualSourceBlendFactor(bc0.alpha_srcblend) ||
	                                                 IsDualSourceBlendFactor(bc0.alpha_destblend)));

	for (uint32_t i = 1; i < 8; i++) {
		const auto& rt = ctx->GetRenderTarget(i);
		if (rt.base.addr != 0 || ps_input_info.target_output_mode[i] != 0 ||
		    ((rt_mask >> (i * 4u)) & 0x0fu) != 0 || ((cb_shader_mask >> (i * 4u)) & 0x0fu) != 0) {
			interesting = true;
		}
	}

	if (!interesting) {
		return;
	}

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx->GetRenderTarget(i);
		const auto& bc  = ctx->GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i], rt.info.format,
		     rt.info.channel_type, rt.info.channel_order, rt.attrib2.width + 1,
		     rt.attrib2.height + 1, rt.attrib3.tile_mode, bc.enable ? "true" : "false",
		     bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend, bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const HW::Context* ctx,
                               const HW::UserConfig*       ucfg,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	if (color.type == RenderColorType::NoColorOutput) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192 && color.type != RenderColorType::DisplayBuffer) {
		return;
	}

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);

	const auto& cc             = ctx->GetColorControl();
	const auto& bc             = ctx->GetBlendControl(color.target_slot);
	const auto& dc             = ctx->GetDepthControl();
	const auto& vp             = ctx->GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });

	VkExtent2D extent = color.vulkan_buffer != nullptr ? color.extent : VkExtent2D {};
	auto       sc     = calc_final_scissor(vp, ctx->GetScanModeControl(), extent);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " clear=%s clear_rgba=(%.3f,%.3f,%.3f,%.3f) cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, GraphicsRunGetFrameNum(), draw_name, RenderColorTypeName(color.type),
	    color.base_addr, extent.width, extent.height, ucfg->GetPrimType(), index_count, flags,
	    ctx->GetRenderTargetMask(), color.color_clear_enable ? "true" : "false",
	    color.color_clear_value.float32[0], color.color_clear_value.float32[1],
	    color.color_clear_value.float32[2], color.color_clear_value.float32[3], cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, ctx, ps_input_info);
}

static void LogDrawInputState(const RenderColorInfo&       color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr, int32_t vertex_offset) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 512 && color.type != RenderColorType::DisplayBuffer) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, GraphicsRunGetFrameNum(), RenderColorTypeName(color.type), color.base_addr,
	     index_type_and_size, index_count, reinterpret_cast<uint64_t>(index_addr),
	     vs_input_info.resources_num, vs_input_info.buffers_num);

	constexpr uint32_t    kIndexSampleLimit  = 64;
	const auto            index_sample_count = std::min(index_count, kIndexSampleLimit);
	std::vector<uint32_t> draw_indices(index_sample_count);
	const bool have_draw_indices = ReadLogIndices(index_type_and_size, index_addr, draw_indices);
	if (have_draw_indices) {
		const auto [min_index, max_index] =
		    std::minmax_element(draw_indices.begin(), draw_indices.end());
		LOGF("DrawInputState[%u]: index_sample=%zu/%u min=%u max=%u vertex_offset=%" PRIi32
		     " effective_min=%" PRIi64 " effective_max=%" PRIi64 "\n",
		     log_id, draw_indices.size(), index_count, *min_index, *max_index, vertex_offset,
		     static_cast<int64_t>(*min_index) + vertex_offset,
		     static_cast<int64_t>(*max_index) + vertex_offset);
		for (uint32_t i = 0; i < draw_indices.size(); i++) {
			LOGF("DrawInputState[%u]: index[%u]=%u effective=%" PRIi64 "\n", log_id, i,
			     draw_indices[i], static_cast<int64_t>(draw_indices[i]) + vertex_offset);
		}
	}

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u attr_num=%d\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index, b.attr_num);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const auto log_record = [&](uint32_t rec) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < b.attr_num; ai++) {
					const auto  res_index = b.attr_indices[ai];
					const auto& r         = vs_input_info.resources[res_index];
					const auto& rd        = vs_input_info.resources_dst[res_index];
					const auto  offset    = b.attr_offsets[ai];
					if (offset < b.stride) {
						uint32_t   attr_raw[4] = {};
						float      attr_flt[4] = {};
						const auto attr_bytes =
						    std::min<uint32_t>(16u, static_cast<uint32_t>(b.stride - offset));
						std::memcpy(attr_raw, rec_bytes + offset, attr_bytes);
						std::memcpy(attr_flt, rec_bytes + offset, attr_bytes);
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=%u "
						     "offset=%u bytes=%u raw=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
						     " %08" PRIx32 " f=(%.6f,%.6f,%.6f,%.6f)\n",
						     log_id, bi, rec, ai, rd.register_start, r.Format(), offset, attr_bytes,
						     attr_raw[0], attr_raw[1], attr_raw[2], attr_raw[3], attr_flt[0],
						     attr_flt[1], attr_flt[2], attr_flt[3]);
					}
					if (offset + 4u <= b.stride &&
					    r.Format() ==
					        Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm)) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			};

			std::vector<uint32_t> logged_records;
			logged_records.reserve(draw_indices.size() + 4u);
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				log_record(rec);
				logged_records.push_back(rec);
			}

			if (have_draw_indices) {
				for (const auto index: draw_indices) {
					const auto draw_offset = (b.fetch_index == 0 ? vertex_offset : 0);
					const auto effective   = static_cast<int64_t>(index) + draw_offset;
					if (effective < 0 || effective >= b.num_records) {
						LOGF("DrawInputState[%u]: vb[%d] effective_index=%" PRIi64
						     " outside records=%u\n",
						     log_id, bi, effective, b.num_records);
						continue;
					}
					const auto rec = static_cast<uint32_t>(effective);
					if (std::find(logged_records.begin(), logged_records.end(), rec) !=
					    logged_records.end()) {
						continue;
					}
					logged_records.push_back(rec);
					log_record(rec);
				}
			}
		}

		for (int ai = 0; ai < b.attr_num; ai++) {
			const auto  res_index = b.attr_indices[ai];
			const auto& r         = vs_input_info.resources[res_index];
			const auto& rd        = vs_input_info.resources_dst[res_index];
			LOGF("DrawInputState[%u]: attr[%d] res=%d offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, res_index, b.attr_offsets[ai], rd.register_start, rd.registers_num,
			     rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

[[maybe_unused]] static void LogDrawTextureState(const char*                 draw_name,
                                                 const RenderColorInfo&      color,
                                                 const ShaderPixelInputInfo& ps_input_info) {
	static std::atomic<uint32_t> log_count {0};
	const auto                   log_id = log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 256) {
		return;
	}

	const auto& ps_program     = *ps_input_info.stage.program;
	const auto& ps_resources   = *ps_input_info.stage.resources;
	const auto  sampled_images = std::count_if(
	    ps_program.info.images.begin(), ps_program.info.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });
	LOGF("DrawTextureState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64 " ps_hash=0x%016" PRIx64
	     " textures=%zu sampled=%zu storage=%zu samplers=%zu\n",
	     log_id, GraphicsRunGetFrameNum(), draw_name, RenderColorTypeName(color.type),
	     color.base_addr, ps_program.shader_hash, ps_program.info.images.size(), sampled_images,
	     ps_program.info.images.size() - sampled_images, ps_program.info.samplers.size());

	for (uint32_t i = 0; i < ps_program.info.images.size(); i++) {
		const auto& image = ps_program.info.images[i];
		const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(ps_resources.images[i]);
		LOGF("DrawTextureState[%u]: tex[%u] source=%u usage=%s sampled=%s "
		     "addr=0x%010" PRIx64 " type=%u fmt=%u extent=%ux%u depth=%u levels=%u base_level=%u "
		     "tile=%u swizzle=0x%03" PRIx32 " fields=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
		     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
		     log_id, i, image.source, image.written ? "read-write" : "read-only",
		     (image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		      image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint)
		         ? "true"
		         : "false",
		     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
		     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
		     static_cast<uint32_t>(r.Depth()) + 1u,
		     std::max<uint32_t>(static_cast<uint32_t>(r.LastLevel()),
		                        static_cast<uint32_t>(r.MaxMip())) +
		         1u,
		     static_cast<uint32_t>(r.BaseLevel()), static_cast<uint32_t>(r.TileMode()),
		     r.DstSelXYZW(), r.fields[0], r.fields[1], r.fields[2], r.fields[3], r.fields[4],
		     r.fields[5], r.fields[6], r.fields[7]);
	}

	for (uint32_t i = 0; i < ps_program.info.samplers.size(); i++) {
		const auto& sampler = ps_program.info.samplers[i];
		const auto  r = DecodeNativeDescriptor<ShaderSamplerResource>(ps_resources.samplers[i]);
		LOGF("DrawTextureState[%u]: samp[%u] source=%u clamp=%u/%u/%u filter=%u/%u/%u "
		     "mip=%u lod=%u-%u"
		     " bias=%d fields=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
		     log_id, i, sampler.source, static_cast<uint32_t>(r.ClampX()),
		     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
		     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
		     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
		     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
		     static_cast<int32_t>(r.LodBias()), r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
	}
}

static void VulkanCmdSetColorWriteEnableEXT(GraphicContext* ctx, VkCommandBuffer command_buffer,
                                            uint32_t        attachment_count,
                                            const VkBool32* p_color_write_enables) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->instance == nullptr);

	static auto func = reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(
	    vkGetInstanceProcAddr(ctx->instance, "vkCmdSetColorWriteEnableEXT"));

	if (func != nullptr) {
		func(command_buffer, attachment_count, p_color_write_enables);
	} else {
		EXIT("vkCmdSetColorWriteEnableEXT not present\n");
	}
}

static PipelineDynamicParameters BuildGraphicsDynamicParams(const HW::Context&     ctx,
                                                            const RenderColorInfo* colors,
                                                            uint32_t               color_count,
                                                            const RenderDepthInfo& depth) {
	EXIT_IF(colors == nullptr);

	PipelineDynamicParameters ret {};
	ret.color_write_count   = color_count;
	ret.stencil_test_enable = depth.stencil_test_enable;

	const auto& vp = ctx.GetScreenViewport();
	VkExtent2D  framebuffer_extent {};
	if (color_count > 0 && colors[0].vulkan_buffer != nullptr) {
		framebuffer_extent = colors[0].extent;
	} else if (depth.vulkan_buffer != nullptr) {
		framebuffer_extent = depth.vulkan_buffer->extent;
	}

	const auto final_scissor = calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent);

	ret.viewport_scale[0]  = vp.viewports[0].xscale;
	ret.viewport_scale[1]  = vp.viewports[0].yscale;
	ret.viewport_scale[2]  = vp.viewports[0].zscale;
	ret.viewport_offset[0] = vp.viewports[0].xoffset;
	ret.viewport_offset[1] = vp.viewports[0].yoffset;
	ret.viewport_offset[2] = vp.viewports[0].zoffset;
	ret.scissor_ltrb[0]    = final_scissor.left;
	ret.scissor_ltrb[1]    = final_scissor.top;
	ret.scissor_ltrb[2]    = final_scissor.right;
	ret.scissor_ltrb[3]    = final_scissor.bottom;
	ret.line_width         = ctx.GetLineWidth();
	ret.stencil_front      = depth.stencil_dynamic_front;
	ret.stencil_back       = depth.stencil_dynamic_back;

	// CB_COLOR_CONTROL.operation controls special CB operations, not the normal color component
	// write mask. Use CB_TARGET_MASK here so scanout passes with mode=Disable do not go black.
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		ret.color_write_enable[i] =
		    (i < color_count &&
		     render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot) != 0);
	}

	return ret;
}

static void SetDynamicParams(VkCommandBuffer                  vk_buffer,
                             const PipelineDynamicParameters& dynamic_params) {
	KYTY_PROFILER_FUNCTION();

	VkViewport viewport {};
	viewport.x        = dynamic_params.viewport_offset[0] - dynamic_params.viewport_scale[0];
	viewport.y        = dynamic_params.viewport_offset[1] - dynamic_params.viewport_scale[1];
	viewport.width    = dynamic_params.viewport_scale[0] * 2.0f;
	viewport.height   = dynamic_params.viewport_scale[1] * 2.0f;
	viewport.minDepth = dynamic_params.viewport_offset[2];
	viewport.maxDepth = dynamic_params.viewport_scale[2] + dynamic_params.viewport_offset[2];
	vkCmdSetViewport(vk_buffer, 0, 1, &viewport);

	VkRect2D scissor {};
	scissor.offset = {dynamic_params.scissor_ltrb[0], dynamic_params.scissor_ltrb[1]};
	scissor.extent = {
	    static_cast<uint32_t>(dynamic_params.scissor_ltrb[2] - dynamic_params.scissor_ltrb[0]),
	    static_cast<uint32_t>(dynamic_params.scissor_ltrb[3] - dynamic_params.scissor_ltrb[1])};
	vkCmdSetScissor(vk_buffer, 0, 1, &scissor);

	float line_width = dynamic_params.line_width;
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	vkCmdSetLineWidth(vk_buffer, line_width);

	if (dynamic_params.stencil_test_enable) {
		vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT,
		                           dynamic_params.stencil_front.compareMask);
		vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT,
		                           dynamic_params.stencil_back.compareMask);
		vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT,
		                         dynamic_params.stencil_front.writeMask);
		vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT,
		                         dynamic_params.stencil_back.writeMask);
		vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_FRONT_BIT,
		                         dynamic_params.stencil_front.reference);
		vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_BACK_BIT,
		                         dynamic_params.stencil_back.reference);
	}

	VkBool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < dynamic_params.color_write_count; i++) {
		enable[i] = (dynamic_params.color_write_enable[i] ? VK_TRUE : VK_FALSE);
	}
	VulkanCmdSetColorWriteEnableEXT(g_render_ctx->GetGraphicCtx(), vk_buffer,
	                                dynamic_params.color_write_count, enable);
}

static bool DrawHasValidVertexShader(const HW::Shader* sh_ctx) {
	EXIT_IF(sh_ctx == nullptr);

	const auto& vs = sh_ctx->GetVs();
	return vs.gs_regs.chksum != 0 && ShaderAddressValid(vs.es_regs.data_addr);
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return sh_regs.shader_z_format != 0 || db.shader_kill_enable || db.shader_z_export_enable ||
	       db.shader_mask_export_enable || db.shader_dual_export_enable ||
	       db.shader_execute_on_noop;
}

static bool ShouldSkipGeShader(const HW::Context* ctx, const HW::UserConfig* ucfg,
                               const HW::Shader* sh_ctx) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(sh_ctx == nullptr);

	const auto& sh_regs     = ctx->GetShaderRegisters();
	const auto& ge_cntl     = ucfg->GetGeControl();
	const auto& vertex_info = sh_ctx->GetVs();
	const auto  stages      = ctx->GetShaderStages();

	const auto is_known_gs_out_prim_type = [](uint32_t value) {
		switch (static_cast<Prospero::GsOutputPrimitiveType>(value)) {
			case Prospero::GsOutputPrimitiveType::kPoints:
			case Prospero::GsOutputPrimitiveType::kLines:
			case Prospero::GsOutputPrimitiveType::kTriangles:
			case Prospero::GsOutputPrimitiveType::k2dRectangle:
			case Prospero::GsOutputPrimitiveType::kRectList: return true;
		}

		return false;
	};

	const bool ps5_ngg_vertex_path = stages == 0x02002000 && vertex_info.es_regs.data_addr != 0 &&
	                                 vertex_info.gs_regs.chksum != 0 &&
	                                 sh_regs.m_vgtGsMaxVertOut == 0x00000000 &&
	                                 is_known_gs_out_prim_type(sh_regs.m_vgtGsOutPrimType);

	const bool unsupported_stage_mask = (stages != 0 && stages != 0x02002000);
	const bool unsupported_gs_stage = (vertex_info.es_regs.data_addr != 0 &&
	                                   vertex_info.gs_regs.data_addr != 0 && !ps5_ngg_vertex_path);
	const bool ge_group_size =
	    ge_cntl.primitive_group_size > 0x0040 || ge_cntl.vertex_group_size > 0x0040;
	const bool ge_shader_regs =
	    (sh_regs.m_geNggSubgrpCntl != 0x00000000 && sh_regs.m_geNggSubgrpCntl != 0x00000001) ||
	    sh_regs.m_vgtGsMaxVertOut != 0x00000000 ||
	    !is_known_gs_out_prim_type(sh_regs.m_vgtGsOutPrimType) ||
	    sh_regs.m_geMaxOutputPerSubgroup > 0x00000040;

	if (unsupported_stage_mask || unsupported_gs_stage || ge_group_size || ge_shader_regs) {
		const auto log_id = g_shader_stage_log_count.fetch_add(1);
		if (log_id < 32) {
			LOGF("Skipping unsupported GE shader draw: stages=0x%08" PRIx32
			     " prim_group=0x%04" PRIx16 " vert_group=0x%04" PRIx16 " ngg=0x%08" PRIx32
			     " max_out=0x%08" PRIx32 " gs_max_vert=0x%08" PRIx32 " gs_out_prim=0x%08" PRIx32
			     " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     stages, ge_cntl.primitive_group_size, ge_cntl.vertex_group_size,
			     sh_regs.m_geNggSubgrpCntl, sh_regs.m_geMaxOutputPerSubgroup,
			     sh_regs.m_vgtGsMaxVertOut, sh_regs.m_vgtGsOutPrimType,
			     vertex_info.es_regs.data_addr, vertex_info.gs_regs.data_addr);
		}
		return true;
	}

	return false;
}

struct DrawRenderState {
	RenderDepthInfo           depth_info;
	RenderColorInfo           color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t                  color_count                              = 0;
	bool                      ps_active                                = true;
	VulkanFramebuffer*        framebuffer                              = nullptr;
	VkCommandBuffer           vk_buffer                                = nullptr;
	ShaderVertexInputInfo     vs_input_info;
	ShaderPixelInputInfo      ps_input_info;
	std::span<const uint32_t> vs_shader;
	std::span<const uint32_t> ps_shader;
	bool                      presentation_viewport = false;
	std::array<float, 2>      presentation_scale {};
	std::array<float, 2>      presentation_offset {};
};

struct DrawCallInfo {
	const char*          name           = nullptr;
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             flags          = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;
};

namespace {

using IrInstruction  = ShaderRecompiler::IR::Instruction;
using IrOpcode       = ShaderRecompiler::IR::Opcode;
using IrOperand      = ShaderRecompiler::IR::Operand;
using IrOperandKind  = ShaderRecompiler::IR::OperandKind;
using IrRegister     = ShaderRecompiler::IR::Register;
using IrRegisterFile = ShaderRecompiler::IR::RegisterFile;

IrOperand IrReg(IrRegisterFile file, uint32_t index, bool negate = false) {
	IrOperand operand {};
	operand.kind   = IrOperandKind::Register;
	operand.reg    = {file, index};
	operand.negate = negate;
	return operand;
}

IrOperand IrImm(uint32_t value) {
	IrOperand operand {};
	operand.kind = IrOperandKind::ImmediateU32;
	operand.imm  = value;
	return operand;
}

bool MatchIr(const IrInstruction& inst, IrOpcode op, const IrOperand& dst,
             std::initializer_list<IrOperand> sources) {
	if (inst.op != op || inst.dst != dst || inst.dst2.kind != IrOperandKind::Null ||
	    inst.src_count != sources.size()) {
		return false;
	}
	size_t index = 0;
	for (const auto& source: sources) {
		if (inst.src[index++] != source) {
			return false;
		}
	}
	return true;
}

const IrInstruction* FindLastIrWrite(const std::vector<IrInstruction>& instructions, size_t before,
                                     const IrOperand& destination) {
	for (size_t i = before; i-- > 0;) {
		if (instructions[i].dst == destination || instructions[i].dst2 == destination) {
			return &instructions[i];
		}
	}
	return nullptr;
}

bool IsInputWrite(const IrInstruction* inst, uint32_t attr, uint32_t channel) {
	return inst != nullptr && inst->op == IrOpcode::LoadInputF32 && inst->src_count == 0 &&
	       inst->input_info.attr == attr && inst->input_info.chan == channel;
}

struct Affine2dProjectionTail {
	uint32_t address_resource = 0;
};

bool MatchAffine2dProjectionTail(const ShaderRecompiler::IR::Program& program,
                                 Affine2dProjectionTail*              proof) {
	if (proof == nullptr || program.blocks.size() != 2 || program.dispatcher_fallback ||
	    program.blocks[0].successors.size() != 1) {
		return false;
	}
	const auto& instructions = program.blocks[0].instructions;
	size_t      export_index = instructions.size();
	for (size_t i = instructions.size(); i-- > 0;) {
		const auto& inst = instructions[i];
		if (inst.op == IrOpcode::Export &&
		    inst.export_info.kind == ShaderRecompiler::IR::ExportTargetKind::Position &&
		    inst.export_info.index == 0 && inst.export_info.en == 0x0f) {
			export_index = i;
			break;
		}
	}
	if (export_index == instructions.size()) {
		return false;
	}

	std::vector<const IrInstruction*> tail;
	for (size_t i = export_index + 1; i-- > 0 && tail.size() < 19;) {
		if (instructions[i].op != IrOpcode::Waitcnt) {
			tail.push_back(&instructions[i]);
		}
	}
	if (tail.size() != 19) {
		return false;
	}
	std::reverse(tail.begin(), tail.end());

	const auto v = [](uint32_t index, bool negate = false) {
		return IrReg(IrRegisterFile::Vector, index, negate);
	};
	const auto vcc = [](uint32_t index) { return IrReg(IrRegisterFile::Vcc, index); };
	const auto s   = [](uint32_t index) { return IrReg(IrRegisterFile::Scalar, index); };
	const auto nil = IrOperand {};
	bool       ok  = true;
	ok &= MatchIr(*tail[0], IrOpcode::CompareGeF32, vcc(0), {IrImm(0), v(14)});
	ok &= MatchIr(*tail[1], IrOpcode::SelectMaskU32, v(9), {vcc(0), IrImm(0x3f800000), IrImm(0)});
	ok &= MatchIr(*tail[2], IrOpcode::SelectMaskU32, v(8), {vcc(0), IrImm(0), v(14)});
	ok &= MatchIr(*tail[3], IrOpcode::FAddF32, v(8), {v(9), v(8)});
	ok &= MatchIr(*tail[4], IrOpcode::ConvertF32ToI32, v(2), {v(9)});
	ok &= MatchIr(*tail[5], IrOpcode::ShiftLeftLogicalU32, v(9), {v(2), IrImm(4)});
	ok &= MatchIr(*tail[6], IrOpcode::RcpF32, v(2), {v(8)});
	ok &= MatchIr(*tail[7], IrOpcode::FlatLoadDword, v(8), {v(9), s(16)});
	ok &= tail[7]->memory.data_dwords == 4 && tail[7]->memory.data_bits == 32 &&
	      tail[7]->memory.offset == 0 && tail[7]->memory.secondary_offset == 0;
	ok &= MatchIr(*tail[8], IrOpcode::FAddF32, v(13), {v(13), v(11)});
	ok &= MatchIr(*tail[9], IrOpcode::FAddF32, v(12), {v(12), v(10)});
	ok &= MatchIr(*tail[10], IrOpcode::FAddF32, v(1), {v(1), v(11)});
	ok &= MatchIr(*tail[11], IrOpcode::FAddF32, v(0), {v(0), v(10)});
	ok &= MatchIr(*tail[12], IrOpcode::FMulF32, v(10), {v(9), v(13)});
	ok &= MatchIr(*tail[13], IrOpcode::FMulF32, v(11), {v(8), v(12)});
	ok &= MatchIr(*tail[14], IrOpcode::FMadF32, v(1), {v(9), v(1), v(10, true)});
	ok &= MatchIr(*tail[15], IrOpcode::FMadF32, v(0), {v(8), v(0), v(11, true)});
	ok &= MatchIr(*tail[16], IrOpcode::FMadF32, v(1), {v(10), v(2), v(1)});
	ok &= MatchIr(*tail[17], IrOpcode::FMadF32, v(0), {v(11), v(2), v(0)});
	ok &= MatchIr(*tail[18], IrOpcode::Export, nil, {v(0), v(1), v(3), v(17)});
	ok &= tail[18]->export_info.kind == ShaderRecompiler::IR::ExportTargetKind::Position &&
	      tail[18]->export_info.index == 0 && tail[18]->export_info.en == 0x0f;
	if (!ok) {
		return false;
	}

	const auto tail_start = static_cast<size_t>(tail[0] - instructions.data());
	if (!IsInputWrite(FindLastIrWrite(instructions, tail_start, v(0)), 0, 0) ||
	    !IsInputWrite(FindLastIrWrite(instructions, tail_start, v(1)), 0, 1) ||
	    !IsInputWrite(FindLastIrWrite(instructions, tail_start, v(2)), 0, 2) ||
	    !IsInputWrite(FindLastIrWrite(instructions, tail_start, v(3)), 0, 3) ||
	    !IsInputWrite(FindLastIrWrite(instructions, tail_start, v(12)), 3, 0) ||
	    !IsInputWrite(FindLastIrWrite(instructions, tail_start, v(13)), 3, 1) ||
	    !IsInputWrite(FindLastIrWrite(instructions, tail_start, v(14)), 3, 2)) {
		return false;
	}
	const auto* w_write = FindLastIrWrite(instructions, tail_start, v(17));
	if (w_write == nullptr || !MatchIr(*w_write, IrOpcode::MoveU32, v(17), {IrImm(0x3f800000)})) {
		return false;
	}
	proof->address_resource = tail[7]->memory.resource;
	return true;
}

struct VertexAttributeView {
	const ShaderVertexInputBuffer* buffer = nullptr;
	uint32_t                       offset = 0;
	uint32_t                       format = 0;
};

bool FindVertexAttribute(const ShaderVertexInputInfo& inputs, uint32_t attr,
                         VertexAttributeView* result) {
	if (result == nullptr) {
		return false;
	}
	bool found = false;
	for (int bi = 0; bi < inputs.buffers_num; bi++) {
		const auto& buffer = inputs.buffers[bi];
		for (int ai = 0; ai < buffer.attr_num; ai++) {
			const auto resource_index = buffer.attr_indices[ai];
			if (resource_index < 0 || resource_index >= inputs.resources_num ||
			    inputs.resources_dst[resource_index].attr_id != static_cast<int>(attr)) {
				continue;
			}
			if (found) {
				return false;
			}
			found          = true;
			result->buffer = &buffer;
			result->offset = buffer.attr_offsets[ai];
			result->format = inputs.resources[resource_index].Format();
		}
	}
	return found;
}

bool ReadVertexFloats(const VertexAttributeView& view, int64_t vertex, uint32_t count,
                      float* values) {
	if (view.buffer == nullptr || values == nullptr || vertex < 0 ||
	    static_cast<uint64_t>(vertex) >= view.buffer->num_records || view.buffer->stride == 0 ||
	    view.offset > view.buffer->stride ||
	    count * sizeof(float) > view.buffer->stride - view.offset) {
		return false;
	}
	const auto address =
	    view.buffer->addr + static_cast<uint64_t>(vertex) * view.buffer->stride + view.offset;
	return Libs::LibKernel::Memory::TryReadBacking(address, values, count * sizeof(float));
}

struct ClipPosition {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 0.0f;
};

bool EvaluateAffine2dPosition(const VertexAttributeView& position,
                              const VertexAttributeView& auxiliary, int64_t vertex,
                              uint64_t constants_address, ClipPosition* result) {
	if (result == nullptr ||
	    position.format != Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float) ||
	    auxiliary.format != Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32Float)) {
		return false;
	}
	float input[4] = {};
	float aux[3]   = {};
	if (!ReadVertexFloats(position, vertex, 4, input) ||
	    !ReadVertexFloats(auxiliary, vertex, 3, aux) || !std::isfinite(aux[2])) {
		return false;
	}
	const bool non_positive = 0.0f >= aux[2];
	const auto byte_offset  = non_positive ? uint64_t {16} : uint64_t {0};
	const auto divisor      = non_positive ? 1.0f : aux[2];
	if (!std::isfinite(divisor) || std::abs(divisor) < std::numeric_limits<float>::epsilon() ||
	    constants_address > UINT64_MAX - byte_offset) {
		return false;
	}
	float constants[4] = {};
	if (!Libs::LibKernel::Memory::TryReadBacking(constants_address + byte_offset, constants,
	                                             sizeof(constants))) {
		return false;
	}
	for (float value: constants) {
		if (!std::isfinite(value)) {
			return false;
		}
	}
	const float inverse = 1.0f / divisor;
	const float term_x  = constants[0] * (aux[0] + constants[2]);
	const float term_y  = constants[1] * (aux[1] + constants[3]);
	result->x           = std::fma(constants[0], input[0] + constants[2], -term_x);
	result->y           = std::fma(constants[1], input[1] + constants[3], -term_y);
	result->x           = std::fma(term_x, inverse, result->x);
	result->y           = std::fma(term_y, inverse, result->y);
	result->z           = input[3];
	result->w           = 1.0f;
	return std::isfinite(result->x) && std::isfinite(result->y) && std::isfinite(result->z);
}

bool ReadSixIndices(uint32_t index_type, const void* address, std::array<uint32_t, 6>* indices) {
	if (address == nullptr || indices == nullptr) {
		return false;
	}
	const auto guest_address = reinterpret_cast<uint64_t>(address);
	switch (static_cast<Prospero::IndexType>(index_type)) {
		case Prospero::IndexType::kIndex8: {
			std::array<uint8_t, 6> raw {};
			if (!Libs::LibKernel::Memory::TryReadBacking(guest_address, raw.data(), raw.size())) {
				return false;
			}
			std::copy(raw.begin(), raw.end(), indices->begin());
			return true;
		}
		case Prospero::IndexType::kIndex16: {
			std::array<uint16_t, 6> raw {};
			if (!Libs::LibKernel::Memory::TryReadBacking(guest_address, raw.data(), sizeof(raw))) {
				return false;
			}
			std::copy(raw.begin(), raw.end(), indices->begin());
			return true;
		}
		case Prospero::IndexType::kIndex32:
			return Libs::LibKernel::Memory::TryReadBacking(guest_address, indices->data(),
			                                               sizeof(*indices));
		default: return false;
	}
}

bool ProvesAxisAlignedQuadCoverage(const std::array<ClipPosition, 6>& clip,
                                   const HW::Viewport& viewport, const ScissorRect& scissor) {
	constexpr float epsilon = 0.01f;
	struct Point {
		float x = 0.0f;
		float y = 0.0f;
	};
	std::array<Point, 6> screen {};
	for (size_t i = 0; i < clip.size(); i++) {
		if (!std::isfinite(clip[i].w) || clip[i].w <= 0.0f || clip[i].z < 0.0f ||
		    clip[i].z > clip[i].w) {
			return false;
		}
		screen[i].x = clip[i].x / clip[i].w * viewport.xscale + viewport.xoffset;
		screen[i].y = clip[i].y / clip[i].w * viewport.yscale + viewport.yoffset;
		if (!std::isfinite(screen[i].x) || !std::isfinite(screen[i].y)) {
			return false;
		}
	}
	std::array<Point, 4> unique {};
	std::array<int, 6>   ids {};
	uint32_t             unique_count = 0;
	for (size_t i = 0; i < screen.size(); i++) {
		ids[i] = -1;
		for (uint32_t p = 0; p < unique_count; p++) {
			if (std::abs(screen[i].x - unique[p].x) <= epsilon &&
			    std::abs(screen[i].y - unique[p].y) <= epsilon) {
				ids[i] = static_cast<int>(p);
				break;
			}
		}
		if (ids[i] < 0) {
			if (unique_count == unique.size()) {
				return false;
			}
			ids[i]                 = static_cast<int>(unique_count);
			unique[unique_count++] = screen[i];
		}
	}
	if (unique_count != 4 || ids[0] == ids[1] || ids[0] == ids[2] || ids[1] == ids[2] ||
	    ids[3] == ids[4] || ids[3] == ids[5] || ids[4] == ids[5]) {
		return false;
	}
	std::array<bool, 4> first {};
	std::array<bool, 4> second {};
	for (uint32_t i = 0; i < 3; i++) {
		first[ids[i]]      = true;
		second[ids[i + 3]] = true;
	}
	std::array<int, 2> shared {};
	uint32_t           shared_count = 0;
	for (uint32_t i = 0; i < 4; i++) {
		if (first[i] && second[i]) {
			if (shared_count == shared.size()) {
				return false;
			}
			shared[shared_count++] = static_cast<int>(i);
		}
	}
	if (shared_count != 2) {
		return false;
	}
	float min_x = unique[0].x;
	float max_x = unique[0].x;
	float min_y = unique[0].y;
	float max_y = unique[0].y;
	for (uint32_t i = 1; i < 4; i++) {
		min_x = std::min(min_x, unique[i].x);
		max_x = std::max(max_x, unique[i].x);
		min_y = std::min(min_y, unique[i].y);
		max_y = std::max(max_y, unique[i].y);
	}
	if (min_x > static_cast<float>(scissor.left) + epsilon ||
	    max_x < static_cast<float>(scissor.right) - epsilon ||
	    min_y > static_cast<float>(scissor.top) + epsilon ||
	    max_y < static_cast<float>(scissor.bottom) - epsilon) {
		return false;
	}
	for (uint32_t i = 0; i < 4; i++) {
		const bool edge_x =
		    std::abs(unique[i].x - min_x) <= epsilon || std::abs(unique[i].x - max_x) <= epsilon;
		const bool edge_y =
		    std::abs(unique[i].y - min_y) <= epsilon || std::abs(unique[i].y - max_y) <= epsilon;
		if (!edge_x || !edge_y) {
			return false;
		}
	}
	return std::abs(unique[shared[0]].x - unique[shared[1]].x) > epsilon &&
	       std::abs(unique[shared[0]].y - unique[shared[1]].y) > epsilon;
}

} // namespace

static bool ProvesFullDisplayOverwrite(const HW::Context& ctx, const HW::UserConfig& ucfg,
                                       const DrawCallInfo& draw, const DrawRenderState& state,
                                       uint32_t index_type, const void* index_address,
                                       int32_t vertex_offset) {
	auto reject = [&](const char* reason) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawCoverageProof: rejected %s\n", reason);
		}
		return false;
	};
	if (state.color_count != 1 || state.color_info[0].type != RenderColorType::DisplayBuffer ||
	    state.color_info[0].vulkan_buffer == nullptr || draw.index_count != 6 ||
	    draw.instance_count != 1 || draw.flags != 0 ||
	    ucfg.GetPrimType() != Prospero::GpuEnumValue(Prospero::PrimitiveType::kTriList)) {
		return reject("draw shape");
	}
	const auto& color = state.color_info[0];
	const auto  slot  = color.target_slot;
	if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
		return reject("invalid color slot");
	}
	const auto  slot_bit = uint32_t {0x0f} << (slot * 4u);
	const auto& rt       = ctx.GetRenderTarget(slot);
	const auto& blend    = ctx.GetBlendControl(slot);
	const auto& depth    = ctx.GetDepthControl();
	const auto& mode     = ctx.GetModeControl();
	const auto& clip     = ctx.GetClipControl();
	const auto& scan     = ctx.GetScanModeControl();
	const auto& eqaa     = ctx.GetEqaaControl();
	const auto& aa       = ctx.GetAaConfig();
	const auto& control  = ctx.GetColorControl();
	const auto& shader   = ctx.GetShaderRegisters();
	if (ctx.GetRenderTargetMask() != slot_bit || shader.m_cbShaderMask != slot_bit ||
	    control.mode != 1 || control.op != 0xcc || blend.enable || depth.z_enable ||
	    depth.z_write_enable || depth.stencil_enable || depth.depth_bounds_enable ||
	    mode.cull_front || mode.cull_back || mode.poly_mode != 0 || scan.msaa_enable ||
	    rt.attrib.num_samples != 0 || rt.attrib.num_fragments != 0 ||
	    eqaa.max_anchor_samples != 0 || eqaa.ps_iter_samples != 0 ||
	    eqaa.mask_export_num_samples != 0 || eqaa.alpha_to_mask_num_samples != 0 ||
	    aa.msaa_num_samples != 0) {
		return reject("destination-dependent fixed-function state");
	}
	if (!clip.dx_clip_space || clip.user_clip_planes != 0 || clip.vertex_kill_any ||
	    clip.min_z_clip_disable || clip.max_z_clip_disable || clip.clip_disable ||
	    clip.force_viewport_index_from_vs_enable) {
		return reject("clip state");
	}
	if (!state.ps_active || !state.ps_input_info.stage ||
	    state.ps_input_info.ps_pixel_kill_enable ||
	    state.ps_input_info.ps_sample_mask_export_enable ||
	    state.ps_input_info.target_output_mode[slot] == 0) {
		return reject("pixel shader coverage");
	}
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		if (i != slot && state.ps_input_info.target_output_mode[i] != 0) {
			return reject("multiple pixel outputs");
		}
	}
	const auto& ps_program   = *state.ps_input_info.stage.program;
	const auto& ps_resources = *state.ps_input_info.stage.resources;
	if (!ps_program.info.buffers.empty() || !ps_program.info.addresses.empty() ||
	    ps_program.info.images.size() != ps_resources.images.size()) {
		return reject("pixel shader address dependency");
	}
	for (uint32_t i = 0; i < ps_program.info.images.size(); i++) {
		const auto& use     = ps_program.info.images[i];
		const auto resource = DecodeNativeDescriptor<ShaderTextureResource>(ps_resources.images[i]);
		const auto address  = resource.Base40();
		if (use.written || use.atomic ||
		    (address >= color.base_addr && address < color.base_addr + color.buffer_size)) {
			return reject("pixel shader destination alias");
		}
	}
	const auto& viewport_state = ctx.GetScreenViewport();
	const auto& viewport       = viewport_state.viewports[0];
	const auto  scissor        = calc_final_scissor(viewport_state, scan, color.extent);
	if (scissor.left != 0 || scissor.top != 0 ||
	    scissor.right != static_cast<int>(color.extent.width) ||
	    scissor.bottom != static_cast<int>(color.extent.height)) {
		return reject("partial scissor");
	}
	if (!state.vs_input_info.stage) {
		return reject("vertex shader runtime");
	}
	const auto&            vs_program   = *state.vs_input_info.stage.program;
	const auto&            vs_resources = *state.vs_input_info.stage.resources;
	Affine2dProjectionTail projection {};
	if (!MatchAffine2dProjectionTail(vs_program, &projection) ||
	    projection.address_resource >= vs_resources.addresses.size()) {
		return reject("unsupported position program");
	}
	VertexAttributeView position {};
	VertexAttributeView auxiliary {};
	if (!FindVertexAttribute(state.vs_input_info, 0, &position) ||
	    !FindVertexAttribute(state.vs_input_info, 3, &auxiliary)) {
		return reject("vertex input mapping");
	}
	std::array<uint32_t, 6> indices {};
	if (!ReadSixIndices(index_type, index_address, &indices)) {
		return reject("index read");
	}
	std::array<ClipPosition, 6> positions {};
	const auto constants = vs_resources.addresses[projection.address_resource].guest_base;
	for (size_t i = 0; i < indices.size(); i++) {
		const auto vertex = static_cast<int64_t>(indices[i]) + static_cast<int64_t>(vertex_offset);
		if (!EvaluateAffine2dPosition(position, auxiliary, vertex, constants, &positions[i])) {
			return reject("position evaluation");
		}
	}
	if (graphics_debug_dump_enabled()) {
		LOGF("DrawCoverageProof: indices=%u,%u,%u,%u,%u,%u "
		     "clip=(%.4f,%.4f,%.4f,%.4f) (%.4f,%.4f,%.4f,%.4f) "
		     "(%.4f,%.4f,%.4f,%.4f) (%.4f,%.4f,%.4f,%.4f) "
		     "(%.4f,%.4f,%.4f,%.4f) (%.4f,%.4f,%.4f,%.4f)\n",
		     indices[0], indices[1], indices[2], indices[3], indices[4], indices[5], positions[0].x,
		     positions[0].y, positions[0].z, positions[0].w, positions[1].x, positions[1].y,
		     positions[1].z, positions[1].w, positions[2].x, positions[2].y, positions[2].z,
		     positions[2].w, positions[3].x, positions[3].y, positions[3].z, positions[3].w,
		     positions[4].x, positions[4].y, positions[4].z, positions[4].w, positions[5].x,
		     positions[5].y, positions[5].z, positions[5].w);
	}
	if (!ProvesAxisAlignedQuadCoverage(positions, viewport, scissor)) {
		return reject("partial vertex coverage");
	}
	LOGF("DrawCoverageProof: full display overwrite proven by affine quad coverage, "
	     "addr=0x%016" PRIx64 " extent=%ux%u constants=0x%016" PRIx64 "\n",
	     color.base_addr, color.extent.width, color.extent.height, constants);
	return true;
}

static bool DrawHasActivePixelShader(const HW::Context* ctx, const HW::Shader* sh_ctx,
                                     const DrawRenderState& state, const DrawCallInfo& draw) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);

	const bool with_depth = (state.depth_info.format != VK_FORMAT_UNDEFINED &&
	                         state.depth_info.vulkan_buffer != nullptr);
	if (state.color_count != 0 || !with_depth) {
		return true;
	}

	const auto& sh_regs = ctx->GetShaderRegisters();
	const auto& ps      = sh_ctx->GetPs();
	return ShaderAddressValid(ps.ps_regs.data_addr) &&
	       PixelShaderHasDepthOrCoverageSideEffects(sh_regs);
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

static bool ConsumeMetadataColorOperation(const HW::Context& ctx) {
	const auto mode = ctx.GetColorControl().mode;
	// These AGC CB modes run color-buffer metadata/decompression operations. The shader is a
	// dummy vehicle for the CB, and its exported color must not be applied as a normal draw.
	// Kyty currently stores host images as expanded Vulkan images and does not track CMASK/DCC
	// metadata state, so the matching host operation is a no-op.
	return mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	       mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	bool     indexed           = false;
	bool     draw_prim7_as_ngg = false;
	uint32_t draw_vertex_count = 0;
	int32_t  vertex_offset     = 0;
	uint32_t first_vertex      = 0;
};

struct DrawIndexBufferSource {
	bool        enabled   = false;
	uint64_t    address   = 0;
	const void* host_data = nullptr;
	uint64_t    size      = 0;
	VkIndexType type      = VK_INDEX_TYPE_UINT16;
};

static void AcquireDisplayColorTarget(const RenderColorInfo& target, VideoOutAccessIntent intent) {
	if (target.type != RenderColorType::DisplayBuffer) {
		return;
	}
	if (target.vulkan_buffer == nullptr || target.extent.width == 0 || target.extent.height == 0) {
		EXIT("Render: invalid display color target acquisition\n");
	}
	VideoOutAccess access {};
	access.intent           = intent;
	access.base_mip_level   = target.base_mip_level;
	access.base_array_layer = 0;
	access.layer_count      = 1;
	access.offset_x         = 0;
	access.offset_y         = 0;
	access.width            = target.extent.width;
	access.height           = target.extent.height;
	g_render_ctx->GetTextureCache()->AcquireVideoOut(
	    static_cast<VideoOutVulkanImage*>(target.vulkan_buffer), access);
}

bool ResolveUnitQuadPresentationViewport(const std::array<std::array<float, 2>, 4>& ndc,
                                         VkExtent2D extent, std::array<float, 2>* scale,
                                         std::array<float, 2>* offset) {
	if (scale == nullptr || offset == nullptr || extent.width == 0 || extent.height == 0) {
		return false;
	}
	for (const auto& point: ndc) {
		if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
			return false;
		}
	}
	const float left   = ndc[0][0];
	const float right  = ndc[1][0];
	const float top    = ndc[0][1];
	const float bottom = ndc[2][1];
	const float span_x = right - left;
	const float span_y = bottom - top;
	if (span_x <= std::numeric_limits<float>::epsilon() ||
	    std::abs(span_y) <= std::numeric_limits<float>::epsilon()) {
		return false;
	}
	const float epsilon = std::max(1.0e-6f, std::max(std::abs(span_x), std::abs(span_y)) * 1.0e-4f);
	const auto  close = [epsilon](float lhs, float rhs) { return std::abs(lhs - rhs) <= epsilon; };
	if (!close(ndc[0][0], left) || !close(ndc[0][1], top) || !close(ndc[1][0], right) ||
	    !close(ndc[1][1], top) || !close(ndc[2][0], left) || !close(ndc[2][1], bottom) ||
	    !close(ndc[3][0], right) || !close(ndc[3][1], bottom)) {
		return false;
	}
	(*scale)[0]  = static_cast<float>(extent.width) / span_x;
	(*scale)[1]  = static_cast<float>(extent.height) / span_y;
	(*offset)[0] = -left * (*scale)[0];
	(*offset)[1] = -top * (*scale)[1];
	return std::isfinite((*scale)[0]) && std::isfinite((*scale)[1]) &&
	       std::isfinite((*offset)[0]) && std::isfinite((*offset)[1]);
}

static bool IsUnitPresentationQuad(const ShaderVertexInputInfo& inputs, uint32_t index_type,
                                   const void* index_address, int32_t vertex_offset) {
	std::array<uint32_t, 6> indices {};
	if (!ReadSixIndices(index_type, index_address, &indices) ||
	    indices != std::array<uint32_t, 6> {0, 1, 2, 1, 3, 2}) {
		return false;
	}
	VertexAttributeView position {};
	if (!FindVertexAttribute(inputs, 0, &position) ||
	    position.format != Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float)) {
		return false;
	}
	constexpr std::array<std::array<float, 2>, 4> expected {
	    {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}}};
	for (uint32_t i = 0; i < expected.size(); i++) {
		float values[2] = {};
		if (!ReadVertexFloats(position, static_cast<int64_t>(i) + vertex_offset, 2, values) ||
		    values[0] != expected[i][0] || values[1] != expected[i][1]) {
			return false;
		}
	}
	return true;
}

static bool TryPrepareDisplayPresentationViewport(const HW::Context&    ctx,
                                                  const HW::UserConfig& ucfg,
                                                  const DrawCallInfo& draw, DrawRenderState* state,
                                                  uint32_t index_type, const void* index_address,
                                                  int32_t vertex_offset) {
	if (state == nullptr || state->color_count != 1 ||
	    state->color_info[0].type != RenderColorType::DisplayBuffer ||
	    state->color_info[0].vulkan_buffer == nullptr || !state->ps_active ||
	    !state->ps_input_info.stage || !state->vs_input_info.stage || draw.index_count != 6 ||
	    draw.instance_count != 1 || draw.flags != 0 || draw.first_instance != 0 ||
	    ucfg.GetPrimType() != Prospero::GpuEnumValue(Prospero::PrimitiveType::kTriList) ||
	    !IsUnitPresentationQuad(state->vs_input_info, index_type, index_address, vertex_offset)) {
		return false;
	}

	const auto& target = state->color_info[0];
	const auto  slot   = target.target_slot;
	if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
		return false;
	}
	const auto  slot_bit       = uint32_t {0x0f} << (slot * 4u);
	const auto& blend          = ctx.GetBlendControl(slot);
	const auto& depth          = ctx.GetDepthControl();
	const auto& control        = ctx.GetColorControl();
	const auto& shader         = ctx.GetShaderRegisters();
	const auto& scan           = ctx.GetScanModeControl();
	const auto& viewport_state = ctx.GetScreenViewport();
	const auto  scissor        = calc_final_scissor(viewport_state, scan, target.extent);
	if (ctx.GetRenderTargetMask() != slot_bit || shader.m_cbShaderMask != slot_bit ||
	    control.mode != 1 || control.op != 0xcc || blend.enable || depth.z_enable ||
	    depth.z_write_enable || depth.stencil_enable || depth.depth_bounds_enable ||
	    state->ps_input_info.ps_pixel_kill_enable ||
	    state->ps_input_info.ps_sample_mask_export_enable ||
	    state->ps_input_info.target_output_mode[slot] == 0 || scissor.left != 0 ||
	    scissor.top != 0 || scissor.right != static_cast<int>(target.extent.width) ||
	    scissor.bottom != static_cast<int>(target.extent.height)) {
		return false;
	}

	const auto& program   = *state->ps_input_info.stage.program;
	const auto& resources = *state->ps_input_info.stage.resources;
	if (program.info.images.size() != 1 || resources.images.size() != 1 ||
	    program.info.samplers.size() != 1 || resources.samplers.size() != 1 ||
	    !program.info.buffers.empty() || !resources.buffers.empty() ||
	    !program.info.addresses.empty() || !resources.addresses.empty()) {
		return false;
	}
	const auto& use = program.info.images[0];
	if (use.kind != ShaderRecompiler::IR::ResourceKind::Image || !use.read || use.written ||
	    use.atomic || use.depth_compare) {
		return false;
	}
	const auto source_descriptor =
	    DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
	const auto source_width  = static_cast<uint32_t>(source_descriptor.Width5()) + 1u;
	const auto source_height = static_cast<uint32_t>(source_descriptor.Height5()) + 1u;
	const auto source_levels = static_cast<uint32_t>(source_descriptor.MaxMip()) + 1u;
	if (source_descriptor.IsNull() || source_width != target.extent.width ||
	    source_height != target.extent.height || source_descriptor.Depth() != 0 ||
	    source_descriptor.BaseLevel() != 0 || source_descriptor.LastLevel() != 0 ||
	    source_levels != 1 || source_descriptor.BaseArray5() != 0 ||
	    source_descriptor.Type() != Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) ||
	    source_descriptor.TileMode() != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
	    source_descriptor.DstSelXYZW() != DstSel(4, 5, 6, 7)) {
		return false;
	}
	if (source_descriptor.Base40() == target.base_addr) {
		return false;
	}

	const auto&            vs_program   = *state->vs_input_info.stage.program;
	const auto&            vs_resources = *state->vs_input_info.stage.resources;
	Affine2dProjectionTail projection {};
	if (!MatchAffine2dProjectionTail(vs_program, &projection) ||
	    projection.address_resource >= vs_resources.addresses.size()) {
		return false;
	}
	VertexAttributeView position {};
	VertexAttributeView auxiliary {};
	if (!FindVertexAttribute(state->vs_input_info, 0, &position) ||
	    !FindVertexAttribute(state->vs_input_info, 3, &auxiliary)) {
		return false;
	}
	std::array<std::array<float, 2>, 4> ndc {};
	const auto constants = vs_resources.addresses[projection.address_resource].guest_base;
	for (uint32_t i = 0; i < ndc.size(); i++) {
		ClipPosition clip {};
		if (!EvaluateAffine2dPosition(position, auxiliary, static_cast<int64_t>(i) + vertex_offset,
		                              constants, &clip) ||
		    !std::isfinite(clip.w) || clip.w <= 0.0f || clip.z < 0.0f || clip.z > clip.w) {
			return false;
		}
		ndc[i] = {clip.x / clip.w, clip.y / clip.w};
	}
	if (!ResolveUnitQuadPresentationViewport(ndc, target.extent, &state->presentation_scale,
	                                         &state->presentation_offset)) {
		return false;
	}
	state->presentation_viewport = true;
	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("Render: expanded unit-quad presentation source=0x%016" PRIx64
		     " destination=0x%016" PRIx64 " extent=%ux%u scale=%.3f,%.3f offset=%.3f,%.3f\n",
		     source_descriptor.Base40(), target.base_addr, source_width, source_height,
		     state->presentation_scale[0], state->presentation_scale[1],
		     state->presentation_offset[0], state->presentation_offset[1]);
	}
	return true;
}

static void FinalizeDrawColorTargetAccess(DrawRenderState*     state,
                                          VideoOutAccessIntent display_intent) {
	EXIT_IF(state == nullptr);
	for (uint32_t i = 0; i < state->color_count; i++) {
		state->color_info[i].color_load_discard =
		    state->color_info[i].type == RenderColorType::DisplayBuffer &&
		    display_intent == VideoOutAccessIntent::FullOverwrite;
		AcquireDisplayColorTarget(state->color_info[i], display_intent);
		MarkRenderTargetGpuWritten(state->color_info[i]);
	}
}

static bool CreateDrawFramebuffer(CommandBuffer* buffer, const DrawCallInfo& draw,
                                  bool skip_null_framebuffer, bool log_setup_phases,
                                  DrawRenderState* state) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);
	if (log_setup_phases) {
		LogDrawPhase(draw.name, "CreateFramebuffer");
	}
	state->framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(
	    state->color_info, state->color_count, &state->depth_info);
	if (state->framebuffer == nullptr && skip_null_framebuffer) {
		return false;
	}
	EXIT_NOT_IMPLEMENTED(state->framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(state->framebuffer->render_pass == nullptr);
	state->vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	return true;
}

static uint64_t VertexBufferDescriptorSize(const ShaderVertexInputBuffer& buffer) {
	return (buffer.stride != 0 ? static_cast<uint64_t>(buffer.stride) * buffer.num_records
	                           : buffer.num_records);
}

static void SetDrawDebugPhase(CommandBuffer* buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(draw.name == nullptr);

	buffer->SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count,
	                     draw.flags, draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig* ucfg, bool auto_draw, bool use_ngg_rectlist_draw,
                            VkPrimitiveTopology* topology) {
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(topology == nullptr);

	*topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

	switch (static_cast<Prospero::PrimitiveType>(ucfg->GetPrimType())) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			*topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			break;
		case Prospero::PrimitiveType::kLineList: *topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
		case Prospero::PrimitiveType::kLineStrip:
			*topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			break;
		case Prospero::PrimitiveType::kTriList:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			break;
		case Prospero::PrimitiveType::kTriFan:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			break;
		case Prospero::PrimitiveType::kRectList:
			*topology = (auto_draw && use_ngg_rectlist_draw ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
			                                                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			if (!auto_draw) {
				EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
			}
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			break;
		default: EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}

	return true;
}

static bool PrepareDrawRenderState(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                                   HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                   const DrawCallInfo& draw, uint32_t render_target_slice_offset,
                                   bool skip_null_framebuffer, bool log_setup_phases,
                                   bool defer_color_target_access, DrawRenderState* state) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);

	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(submit_id, buffer, *ctx, &state->depth_info);

	if (ResolveColorTargets(submit_id, buffer, *ctx, render_target_slice_offset)) {
		MarkRenderTargetGpuWritten(state->depth_info);
		return false;
	}
	MarkRenderTargetGpuWritten(state->depth_info);

	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if (slot == 0 || (render_target_mask_slot(ctx->GetRenderTargetMask(), slot) != 0 &&
		                  ctx->GetRenderTarget(slot).base.addr != 0)) {
			ResolveRenderColorTarget(submit_id, buffer, *ctx,
			                         &state->color_info[state->color_count],
			                         render_target_slice_offset, slot);
			if (state->color_info[state->color_count].vulkan_buffer != nullptr) {
				if (!defer_color_target_access) {
					AcquireDisplayColorTarget(state->color_info[state->color_count],
					                          VideoOutAccessIntent::Preserve);
					MarkRenderTargetGpuWritten(state->color_info[state->color_count]);
				}
				state->color_count++;
			}
		}
	}

	const bool with_depth = (state->depth_info.format != VK_FORMAT_UNDEFINED &&
	                         state->depth_info.vulkan_buffer != nullptr);
	if (state->color_count == 0 && !with_depth) {
		LogFramebufferSkip(draw.name, state->color_info[0], state->depth_info, ctx, ucfg,
		                   draw.index_count, draw.flags);
		return false;
	}
	state->ps_active = DrawHasActivePixelShader(ctx, sh_ctx, *state, draw);
	if (defer_color_target_access) {
		return true;
	}
	if (!CreateDrawFramebuffer(buffer, draw, skip_null_framebuffer, log_setup_phases, state)) {
		LogFramebufferSkip(draw.name, state->color_info[0], state->depth_info, ctx, ucfg,
		                   draw.index_count, draw.flags);
		return false;
	}
	return true;
}

static void RefreshShaders(HW::Context* ctx, HW::Shader* sh_ctx, const DrawCallInfo& draw,
                           bool log_phases, DrawRenderState* state) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);

	const auto& vertex_shader_info = sh_ctx->GetVs();
	const auto& pixel_shader_info  = sh_ctx->GetPs();
	const auto& shader_regs        = ctx->GetShaderRegisters();

	state->vs_shader     = {};
	state->ps_shader     = {};
	state->ps_input_info = {};
	EXIT_IF(g_render_ctx == nullptr || g_render_ctx->GetGraphicCtx() == nullptr);
	const auto lane_mask_mode = SelectGraphicsLaneMaskMode(*g_render_ctx->GetGraphicCtx(), 64u);

	if (log_phases) {
		LogDrawPhase(draw.name, "ShaderCompileInfoVS");
	}
	if (!ShaderCompileInfoVS(&vertex_shader_info, &shader_regs, lane_mask_mode,
	                         &state->vs_input_info, &state->vs_shader)) {
		EXIT("ShaderCompileInfoVS failed for draw %s\n", draw.name);
	}

	if (!state->ps_active) {
		return;
	}
	if (log_phases) {
		LogDrawPhase(draw.name, "ShaderCompileInfoPS");
	}
	if (!ShaderCompileInfoPS(&pixel_shader_info, &shader_regs, lane_mask_mode,
	                         &state->vs_input_info, &state->ps_input_info, &state->ps_shader)) {
		EXIT("ShaderCompileInfoPS failed for draw %s\n", draw.name);
	}
}

static void BindDrawVertexBuffers(uint64_t submit_id, CommandBuffer* buffer,
                                  const DrawCallInfo& draw, VkCommandBuffer vk_buffer,
                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(draw.name == nullptr);
	(void)submit_id;

	LogDrawPhase(draw.name, "BindVertexBuffers");
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto&   b        = vs_input_info.buffers[i];
		uint64_t      addr     = b.addr;
		uint64_t      size     = VertexBufferDescriptorSize(b);
		VulkanBuffer* vertices = nullptr;
		VkDeviceSize  offset   = 0;

		if (size == 0) {
			vertices = g_render_ctx->GetBufferCache()->ObtainNullBuffer(
			    buffer, g_render_ctx->GetGraphicCtx());
		} else {
			auto binding = g_render_ctx->GetBufferCache()->ObtainBuffer(
			    buffer, g_render_ctx->GetGraphicCtx(), addr, size);
			vertices = binding.first;
			offset   = binding.second;
		}
		EXIT_NOT_IMPLEMENTED(vertices == nullptr);

		vkCmdBindVertexBuffers(vk_buffer, i, 1, &vertices->buffer, &offset);
	}
}

static void BindDrawIndexBuffer(CommandBuffer* buffer, VkCommandBuffer vk_buffer,
                                const DrawIndexBufferSource& source) {
	if (!source.enabled) {
		return;
	}
	EXIT_IF(source.size == 0);

	VulkanBuffer* index_buffer = nullptr;
	VkDeviceSize  index_offset = 0;
	if (source.host_data != nullptr) {
		VkDeviceSize range = 0;
		if (!g_render_ctx->GetBufferCache()->UploadHostData(
		        buffer, g_render_ctx->GetGraphicCtx(), source.host_data, source.size, 16,
		        &index_buffer, &index_offset, &range)) {
			EXIT("failed to upload host index buffer\n");
		}
	} else {
		auto binding = g_render_ctx->GetBufferCache()->ObtainBuffer(
		    buffer, g_render_ctx->GetGraphicCtx(), source.address, source.size);
		index_buffer = binding.first;
		index_offset = binding.second;
	}
	EXIT_IF(index_buffer == nullptr);
	vkCmdBindIndexBuffer(vk_buffer, index_buffer->buffer, index_offset, source.type);
}

static void LogDrawStateIfNeeded(HW::Context* ctx, HW::UserConfig* ucfg, const DrawCallInfo& draw,
                                 const DrawRenderState& state, bool always_log,
                                 bool force_legacy_rect_log, uint32_t index_type_and_size,
                                 const void* index_addr, int32_t vertex_offset) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(draw.name == nullptr);

	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!always_log && !force_legacy_rect_log) {
		return;
	}

	LogDrawTargetState(draw.name, state.color_info[0], state.depth_info, ctx, ucfg,
	                   state.ps_input_info, draw.index_count, draw.flags);
	LogDrawInputState(state.color_info[0], state.vs_input_info, index_type_and_size,
	                  draw.index_count, index_addr, vertex_offset);
	if (state.ps_active && state.ps_input_info.stage) {
		LogDrawTextureState(draw.name, state.color_info[0], state.ps_input_info);
	}
}

static bool IsHostExpandedRectListDrawSupported(const ShaderVertexInputInfo& vs_input_info,
                                                const DrawCallInfo&          draw,
                                                const DrawEmitInfo&          emit) {
	if (!emit.draw_prim7_as_ngg) {
		return true;
	}

	if (vs_input_info.buffers_num != 0) {
		return false;
	}

	return draw.index_count == 3 || draw.index_count == emit.draw_vertex_count;
}

static void EmitDrawPrimitives(const HW::UserConfig* ucfg, VkCommandBuffer vk_buffer,
                               const ShaderVertexInputInfo& vs_input_info, const DrawCallInfo& draw,
                               const DrawEmitInfo& emit) {
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(draw.name == nullptr);
	switch (static_cast<Prospero::PrimitiveType>(ucfg->GetPrimType())) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
			if (emit.indexed) {
				vkCmdDrawIndexed(vk_buffer, draw.index_count, draw.instance_count, 0,
				                 emit.vertex_offset, draw.first_instance);
			} else {
				vkCmdDraw(vk_buffer, draw.index_count, draw.instance_count, emit.first_vertex,
				          draw.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kRectList:
			if (emit.indexed) {
				vkCmdDrawIndexed(vk_buffer, draw.index_count, draw.instance_count, 0,
				                 emit.vertex_offset, draw.first_instance);
			} else {
				EXIT_NOT_IMPLEMENTED(
				    !IsHostExpandedRectListDrawSupported(vs_input_info, draw, emit));
				vkCmdDraw(vk_buffer, emit.draw_vertex_count, draw.instance_count, emit.first_vertex,
				          draw.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			if (emit.indexed) {
				EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
			}
			// Sarah
			EXIT_NOT_IMPLEMENTED(!(draw.index_count == 3 && vs_input_info.buffers_num == 0));
			vkCmdDraw(vk_buffer, 4, draw.instance_count, emit.first_vertex, draw.first_instance);
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (emit.indexed) {
					vkCmdDrawIndexed(vk_buffer, 4, draw.instance_count, i, emit.vertex_offset,
					                 draw.first_instance);
				} else {
					vkCmdDraw(vk_buffer, 4, draw.instance_count, i + emit.first_vertex,
					          draw.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}
}

static void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                                HW::UserConfig* ucfg, HW::Shader* sh_ctx, const DrawCallInfo& draw,
                                DrawRenderState* state, VkPrimitiveTopology topology,
                                const DrawEmitInfo& emit, const DrawIndexBufferSource& index_source,
                                bool log_pipeline_phase, bool set_bind_debug, bool set_auto_debug) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(sh_ctx == nullptr);
	EXIT_IF(draw.name == nullptr);
	EXIT_IF(state == nullptr);

	for (;;) {
		const auto recording_generation = buffer->GetRecordingGeneration();
		if (log_pipeline_phase) {
			LogDrawPhase(draw.name, "CreatePipeline");
		}
		auto* pipeline = g_render_ctx->GetPipelineCache()->CreateGraphicsPipeline(
		    state->framebuffer, state->color_info, state->color_count, &state->depth_info,
		    &state->vs_input_info, ctx, sh_ctx, &state->ps_input_info, topology, state->ps_active,
		    state->vs_shader, state->ps_shader);

		if (set_bind_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x100u);
		}
		vkCmdBindPipeline(state->vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
		auto dynamic_params = BuildGraphicsDynamicParams(
		    *ctx, state->color_info, state->color_count, state->depth_info);
		if (state->presentation_viewport) {
			dynamic_params.viewport_scale[0]  = state->presentation_scale[0];
			dynamic_params.viewport_scale[1]  = state->presentation_scale[1];
			dynamic_params.viewport_offset[0] = state->presentation_offset[0];
			dynamic_params.viewport_offset[1] = state->presentation_offset[1];
		}
		SetDynamicParams(state->vk_buffer, dynamic_params);

		// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);
		BindDrawVertexBuffers(submit_id, buffer, draw, state->vk_buffer, state->vs_input_info);

		LogDrawPhase(draw.name, "BindDescriptorsVS");
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x200u);
		}
		const auto vs_address_writes = BindDescriptors(
		    submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout,
		    state->vs_input_info.stage, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

		std::vector<ShaderAddressWriteRange> ps_address_writes;
		if (state->ps_active) {
			LogDrawPhase(draw.name, "BindDescriptorsPS");
			if (set_auto_debug) {
				SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
			}
			ps_address_writes =
			    BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                    pipeline->pipeline_layout, state->ps_input_info.stage,
			                    VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);
		}
		if (buffer->GetRecordingGeneration() != recording_generation) {
			continue;
		}
		// Index data may use this command buffer's host stream. Resolve it only after the other
		// fault-capable bindings, and rebuild it whenever a fault reset changes the generation.
		BindDrawIndexBuffer(buffer, state->vk_buffer, index_source);
		if (buffer->GetRecordingGeneration() != recording_generation) {
			continue;
		}

		LogDrawPhase(draw.name, "BeginRenderPass");
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
		}
		buffer->BeginRenderPass(state->framebuffer, state->color_info, state->color_count,
		                        &state->depth_info);
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
		}

		EmitDrawPrimitives(ucfg, state->vk_buffer, state->vs_input_info, draw, emit);

		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
		}
		buffer->EndRenderPass();
		VkPipelineStageFlags shader_write_stages = 0;
		const bool           vs_wrote_buffers   = HasShaderBufferWrites(state->vs_input_info.stage);
		const bool           vs_wrote_addresses = MarkShaderAddressWrites(vs_address_writes);
		if (vs_wrote_buffers || vs_wrote_addresses) {
			shader_write_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		}
		if (state->ps_active) {
			const bool ps_wrote_buffers   = HasShaderBufferWrites(state->ps_input_info.stage);
			const bool ps_wrote_addresses = MarkShaderAddressWrites(ps_address_writes);
			if (ps_wrote_buffers || ps_wrote_addresses) {
				shader_write_stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			}
		}
		if (shader_write_stages != 0) {
			ShaderWriteBarrier(state->vk_buffer, shader_write_stages);
		}
		LogDrawPhase(draw.name, "EndRenderPass");
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
		}
		break;
	}
}

void RenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                     HW::UserConfig* ucfg, HW::Shader* sh_ctx, uint32_t index_type_and_size,
                     uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                     uint32_t instance_count, uint32_t render_target_slice_offset,
                     int32_t vertex_offset_add, uint32_t first_instance) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                     index_count, flags, type, instance_count,
	                     reinterpret_cast<uint64_t>(index_addr));

	Common::LockGuard lock(g_render_ctx->GetMutex());

	if (index_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(*ctx)) {
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (ShouldSkipGeShader(ctx, ucfg, sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndex():Shader:", *sh_ctx);
		uc_print("GraphicsRenderDrawIndex():UserConfig:", *ucfg);
		hw_print(*ctx);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t flags               = 0x%08" PRIx32 "\n"
		     "\t type                = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t rt_slice_offset     = 0x%08" PRIx32 "\n"
		     "\t vertex_offset_add   = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     index_type_and_size, index_count, reinterpret_cast<uint64_t>(index_addr), flags, type,
		     instance_count, render_target_slice_offset, static_cast<uint32_t>(vertex_offset_add),
		     first_instance);
	}
	sh_check(*sh_ctx);

	uc_check(*ucfg);

	hw_check(*ctx);

	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	if (!GetDrawTopology(ucfg, false, false, &topology)) {
		return;
	}

	VkIndexType index_type           = VK_INDEX_TYPE_UINT16;
	uint64_t    index_size           = 0;
	bool        expand_index8_to_u16 = false;

	switch (static_cast<Prospero::IndexType>(index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_type = VK_INDEX_TYPE_UINT16;
			index_size = 2 * static_cast<uint64_t>(index_count);
			break;
		case Prospero::IndexType::kIndex32:
			index_type = VK_INDEX_TYPE_UINT32;
			index_size = 4 * static_cast<uint64_t>(index_count);
			break;
		// Some games use it - need vulkan extension
		case Prospero::IndexType::kIndex8:
			index_type           = VK_INDEX_TYPE_UINT16;
			index_size           = static_cast<uint64_t>(index_count);
			expand_index8_to_u16 = true;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", index_type_and_size);
	}

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(type != 1);
	if (instance_count == 0) {
		instance_count = 1;
	}

	const DrawCallInfo draw {"DrawIndex",    CommandBufferDebugOp::DrawIndex,
	                         index_count,    flags,
	                         instance_count, first_instance};
	std::vector<uint16_t> expanded_indices;
	if (expand_index8_to_u16) {
		EXIT_NOT_IMPLEMENTED(index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(index_addr);
		expanded_indices.resize(index_count);
		for (uint32_t i = 0; i < index_count; i++) {
			expanded_indices[i] = src[i];
		}
	}

	DrawIndexBufferSource index_source {};
	index_source.enabled = true;
	index_source.address = reinterpret_cast<uint64_t>(index_addr);
	index_source.host_data =
	    expanded_indices.empty() ? nullptr : static_cast<const void*>(expanded_indices.data());
	index_source.size = expanded_indices.empty()
	                        ? index_size
	                        : expanded_indices.size() * sizeof(uint16_t);
	index_source.type = index_type;

	DrawRenderState state {};
	if (!PrepareDrawRenderState(submit_id, buffer, ctx, ucfg, sh_ctx, draw,
	                            render_target_slice_offset, false, true, true, &state)) {
		return;
	}

	RefreshShaders(ctx, sh_ctx, draw, true, &state);

	const auto vertex_offset =
	    ResolveVertexOffset(ucfg->GetIndexOffset(), state.vs_input_info) + vertex_offset_add;
	LogDrawStateIfNeeded(ctx, ucfg, draw, state, true, false, index_type_and_size, index_addr,
	                     vertex_offset);
	const bool presentation_viewport = TryPrepareDisplayPresentationViewport(
	    *ctx, *ucfg, draw, &state, index_type_and_size, index_addr, vertex_offset);
	const bool full_display_overwrite =
	    presentation_viewport ||
	    ProvesFullDisplayOverwrite(*ctx, *ucfg, draw, state, index_type_and_size, index_addr,
	                               vertex_offset);
	const auto display_intent = full_display_overwrite ? VideoOutAccessIntent::FullOverwrite
	                                                   : VideoOutAccessIntent::Preserve;
	FinalizeDrawColorTargetAccess(&state, display_intent);
	if (!CreateDrawFramebuffer(buffer, draw, false, true, &state)) {
		return;
	}

	DrawEmitInfo emit {};
	emit.indexed       = true;
	emit.vertex_offset = vertex_offset;

	ExecutePreparedDraw(submit_id, buffer, ctx, ucfg, sh_ctx, draw, &state, topology, emit,
	                    index_source, true, true, false);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                         HW::UserConfig* ucfg, HW::Shader* sh_ctx, uint32_t index_count,
                         uint32_t flags, uint32_t render_target_slice_offset,
                         uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr || ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                     index_count, flags, first_vertex, instance_count, first_instance);

	Common::LockGuard lock(g_render_ctx->GetMutex());

	if (index_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(*ctx)) {
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (ShouldSkipGeShader(ctx, ucfg, sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndexAuto():Shader:", *sh_ctx);
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", *ucfg);
		hw_print(*ctx);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t flags               = 0x%08" PRIx32 "\n"
		     "\t rt_slice_offset     = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     index_count, flags, render_target_slice_offset, instance_count, first_vertex,
		     first_instance);
	}
	sh_check(*sh_ctx);

	uc_check(*ucfg);

	hw_check(*ctx);

	EXIT_NOT_IMPLEMENTED(flags != 0);
	if (instance_count == 0) {
		instance_count = 1;
	}

	const DrawCallInfo draw {"DrawIndexAuto", CommandBufferDebugOp::DrawIndexAuto,
	                         index_count,     flags,
	                         instance_count,  first_instance};

	DrawRenderState state {};
	if (!PrepareDrawRenderState(submit_id, buffer, ctx, ucfg, sh_ctx, draw,
	                            render_target_slice_offset, true, false, false, &state)) {
		return;
	}

	VkPrimitiveTopology topology              = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	const bool          use_ngg_rectlist_draw = Config::NggRectlistDrawEnabled();

	if (!GetDrawTopology(ucfg, true, use_ngg_rectlist_draw, &topology)) {
		return;
	}
	const bool draw_prim7_as_ngg =
	    (use_ngg_rectlist_draw &&
	     ucfg->GetPrimType() == Prospero::GpuEnumValue(Prospero::PrimitiveType::kRectList));

	RefreshShaders(ctx, sh_ctx, draw, false, &state);

	if (draw_prim7_as_ngg && state.vs_input_info.buffers_num == 0 &&
	    state.vs_input_info.param_export_mask == 0 && state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx->GetPs().ps_regs.chksum,
			     sh_ctx->GetVs().es_regs.data_addr, sh_ctx->GetVs().gs_regs.data_addr);
		}
		return;
	}

	const uint32_t draw_vertex_count = (draw_prim7_as_ngg ? 4u : index_count);
	const auto vertex_offset = ResolveVertexOffset(ucfg->GetIndexOffset(), state.vs_input_info) +
	                           static_cast<int32_t>(first_vertex);
	LogDrawStateIfNeeded(ctx, ucfg, draw, state, false,
	                     ucfg->GetPrimType() ==
	                         Prospero::GpuEnumValue(Prospero::PrimitiveType::kRectListLegacy),
	                     0, nullptr, vertex_offset);
	DrawEmitInfo emit {};
	emit.draw_prim7_as_ngg = draw_prim7_as_ngg;
	emit.draw_vertex_count = draw_vertex_count;
	emit.first_vertex      = static_cast<uint32_t>(vertex_offset);

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, ctx, ucfg, sh_ctx, draw, &state, topology, emit,
	                    index_source, false, false, true);
}

bool IsSameColorResolveSubresource(const RenderColorInfo& src, const RenderColorInfo& dst) {
	return src.base_addr == dst.base_addr && src.base_mip_level == dst.base_mip_level &&
	       src.base_array_layer == dst.base_array_layer;
}

ImageImageCopy MakeColorResolveCopy(const RenderColorInfo& src, const RenderColorInfo& dst,
                                    uint32_t width, uint32_t height) {
	ImageImageCopy region {};
	region.src_image = src.vulkan_buffer;
	region.src_level = src.base_mip_level;
	region.dst_level = dst.base_mip_level;
	region.width     = width;
	region.height    = height;
	region.src_layer = src.base_array_layer;
	region.dst_layer = dst.base_array_layer;
	return region;
}

static bool ResolveColorTargets(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                                uint32_t render_target_slice_offset) {
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(submit_id, buffer, hw, &src, render_target_slice_offset, 0, true,
	                         true);
	ResolveRenderColorTarget(submit_id, buffer, hw, &dst, render_target_slice_offset, 1, true);
	if (src.vulkan_buffer == nullptr || dst.vulkan_buffer == nullptr ||
	    src.type == RenderColorType::NoColorOutput || dst.type == RenderColorType::NoColorOutput) {
		return false;
	}
	AcquireDisplayColorTarget(src, VideoOutAccessIntent::Preserve);
	if (IsSameColorResolveSubresource(src, dst)) {
		return true;
	}

	const uint32_t width  = std::min(src.extent.width, dst.extent.width);
	const uint32_t height = std::min(src.extent.height, dst.extent.height);
	if (width == 0 || height == 0) {
		return false;
	}
	const bool full_destination =
	    dst.base_mip_level == 0 && width == dst.extent.width && height == dst.extent.height &&
	    src.extent.width == dst.extent.width && src.extent.height == dst.extent.height &&
	    src.format == dst.format;
	AcquireDisplayColorTarget(dst, full_destination ? VideoOutAccessIntent::FullOverwrite
	                                                : VideoOutAccessIntent::Preserve);

	const std::array regions {MakeColorResolveCopy(src, dst, width, height)};

	UtilImageToImage(buffer, regions, dst.vulkan_buffer, dst.vulkan_buffer->layout);
	MarkRenderTargetGpuWritten(dst);
	return true;
}

} // namespace Libs::Graphics
