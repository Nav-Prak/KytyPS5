#pragma once

// Vulkan-Headers no longer ships the C enum string helper used by Kyty. Keep the existing
// call sites source-compatible without coupling the C API build to vulkan.hpp.

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vulkan/vulkan.h>

namespace Kyty::VulkanStrings {

[[nodiscard]] inline const char* EnumString(const char* type, int64_t value) {
	// A small ring keeps pointers from multiple conversions in one logging expression valid.
	thread_local std::array<std::string, 8> strings;
	thread_local size_t                     next = 0;
	auto&                                   result = strings[next++ % strings.size()];
	result = std::string(type) + '(' + std::to_string(value) + ')';
	return result.c_str();
}

} // namespace Kyty::VulkanStrings

[[nodiscard]] inline const char* string_VkResult(VkResult value) {
	return Kyty::VulkanStrings::EnumString("VkResult", static_cast<int64_t>(value));
}

[[nodiscard]] inline const char* string_VkFormat(VkFormat value) {
	return Kyty::VulkanStrings::EnumString("VkFormat", static_cast<int64_t>(value));
}

[[nodiscard]] inline const char* string_VkImageLayout(VkImageLayout value) {
	return Kyty::VulkanStrings::EnumString("VkImageLayout", static_cast<int64_t>(value));
}

[[nodiscard]] inline std::string string_VkQueueFlags(VkQueueFlags value) {
	return "VkQueueFlags(" + std::to_string(static_cast<uint64_t>(value)) + ')';
}
