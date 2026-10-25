/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "utils.hpp"
#include "lsfg_extent.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::backend;

ConstantBuffer backend::getDefaultConstantBuffer(
        size_t index, size_t total, float invFlow) {
    return ConstantBuffer {
        .resolutionInvScale = invFlow,
        .timestamp = static_cast<float>(index + 1) / static_cast<float>(total + 1),
        .uiThreshold = 0.5F
    };
}

VkExtent2D backend::shift_extent(VkExtent2D extent, uint32_t i) {
    const LsfgMipExtent mip = lsfg_mip_extent(extent.width, extent.height, i);
    return VkExtent2D{
        .width = mip.width,
        .height = mip.height
    };
}

VkExtent2D backend::add_shift_extent(VkExtent2D extent, uint32_t a, uint32_t i) {
    return VkExtent2D{
        .width = (extent.width + a) >> i,
        .height = (extent.height + a) >> i
    };
}

std::string backend::to_hex_id(uint32_t id) {
    const std::array<char, 17> chars = std::to_array("0123456789ABCDEF");

    std::string result = "0x";
    result += chars.at((id >> 12) & 0xF);
    result += chars.at((id >> 8) & 0xF);
    result += chars.at((id >> 4) & 0xF);
    result += chars.at(id & 0xF);
    return result;
}
