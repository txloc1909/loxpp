#pragma once

// Shared VM capacity limits. vm.h and profiler.h both need the frame budget,
// but vm.h includes profiler.h under LOXPP_PROFILE, so profiler.h cannot
// include vm.h to read VM::FRAMES_MAX. This header is the single source of
// truth both include instead.
namespace loxpp {

inline constexpr int kFramesMax = 1024;
inline constexpr int kStackMax = 16384;
inline constexpr int kStackOverflowFrameReserve = 16;
inline constexpr int kStackOverflowStackReserve = 64;

} // namespace loxpp
