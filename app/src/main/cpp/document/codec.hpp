#pragma once

#include "document.hpp"
#include <span>

namespace engine::document::wire {
// Fixed-width v1; no native structs, pointers, tessellation or padding on disk.
constexpr size_t sampleBytes = 36, chunkSamples = 1820, headerBytes = 80;
using Header = std::array<std::uint8_t,headerBytes>;
bool header(const Brush&, double dx, double dy, Header&) noexcept;
bool readHeader(std::span<const std::uint8_t>, Brush&, double& dx, double& dy) noexcept;
bool samples(std::span<const input::RawInputPoint>, std::span<std::uint8_t>) noexcept;
// Reject the complete slice on malformed input. previousTime = -1 on first chunk.
bool readSamples(std::span<const std::uint8_t>, double previousTime,
                 std::span<input::RawInputPoint>) noexcept;
}
