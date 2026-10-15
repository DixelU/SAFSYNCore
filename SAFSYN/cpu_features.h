#pragma once

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace safsyn::detail
{
// Both the processor and the OS must support saving XMM/YMM state. Keep this
// probe in baseline code; compiling the whole engine for AVX2 is not safe.
inline bool avx2_available() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	int registers[4];
	__cpuidex(registers, 0, 0);
	if (registers[0] < 7) return false;
	__cpuidex(registers, 1, 0);
	constexpr int avx_and_osxsave = (1 << 28) | (1 << 27);
	if ((registers[2] & avx_and_osxsave) != avx_and_osxsave) return false;
	if ((_xgetbv(0) & 6) != 6) return false;
	__cpuidex(registers, 7, 0);
	return (registers[1] & (1 << 5)) != 0;
#elif defined(__x86_64__) || defined(__i386__)
	// GCC/Clang's CPU probe includes the OS extended-state check.
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2");
#else
	return false;
#endif
}
} // namespace safsyn::detail
