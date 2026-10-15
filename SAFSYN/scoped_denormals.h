#pragma once

#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#endif

namespace safsyn::detail
{
// Recursive filters can decay into subnormal floats and become much slower
// while processing silence. Flush those inaudible values during mixing only;
// restore the caller's floating-point environment on every return path.
class ScopedNoDenormals
{
#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
	unsigned previous_ = _mm_getcsr();
public:
	ScopedNoDenormals() noexcept { _mm_setcsr(previous_ | 0x8040u); }
	~ScopedNoDenormals() { _mm_setcsr(previous_); }
#else
public:
	ScopedNoDenormals() noexcept = default;
#endif
	ScopedNoDenormals(const ScopedNoDenormals&) = delete;
	ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;
};
} // namespace safsyn::detail
