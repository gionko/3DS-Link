#pragma once
#include <array>
#include "common/common_types.h"
namespace HW::AES {
enum KeySlotID : std::size_t { UDSDataKey = 0x2D };
inline std::array<u8, 16> GetNormalKey(std::size_t) { return {}; }
} // namespace HW::AES
