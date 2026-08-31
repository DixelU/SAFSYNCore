#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // Intentional cache-line separation of queue indices.
#endif

namespace safsyn::detail
{

// Bounded sequence-number queue: multiple producers, single consumer. A
// preempted producer can temporarily stall consumption, never the audio thread.
template<class T> class MidiQueue
{
	struct Cell { std::atomic<uint64_t> sequence{0}; T value{}; };
	std::unique_ptr<Cell[]> cells_;
	size_t capacity_;
	alignas(64) std::atomic<uint64_t> write_{0};
	alignas(64) uint64_t read_ = 0;
public:
	explicit MidiQueue(size_t capacity) : cells_(std::make_unique<Cell[]>(capacity)),
		capacity_(capacity)
	{
		if (capacity < 2) throw std::invalid_argument("MIDI queue capacity must be >= 2");
		for (size_t i = 0; i < capacity; ++i) cells_[i].sequence.store(i);
	}
	bool push(const T& value) noexcept
	{
		uint64_t position = write_.load(std::memory_order_relaxed);
		for (;;)
		{
			auto& cell = cells_[position % capacity_];
			const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
			if (sequence == position)
			{
				if (write_.compare_exchange_weak(position, position + 1,
					std::memory_order_relaxed))
				{
					cell.value = value;
					cell.sequence.store(position + 1, std::memory_order_release);
					return true;
				}
			}
			else if (sequence < position) return false;
			else position = write_.load(std::memory_order_relaxed);
		}
	}
	bool pop(T& value) noexcept
	{
		auto& cell = cells_[read_ % capacity_];
		if (cell.sequence.load(std::memory_order_acquire) != read_ + 1) return false;
		value = cell.value;
		cell.sequence.store(read_ + capacity_, std::memory_order_release);
		++read_;
		return true;
	}
};

// Stereo frame ring. Read/write are exclusively owned by consumer/producer.
class AudioRing
{
	std::vector<float> audio_;
	size_t capacity_;
	alignas(64) std::atomic<uint64_t> write_{0};
	alignas(64) std::atomic<uint64_t> read_{0};
public:
	explicit AudioRing(size_t frames) : audio_(frames * 2), capacity_(frames)
	{
		if (!frames) throw std::invalid_argument("empty audio ring");
	}
	size_t capacity() const noexcept { return capacity_; }
	size_t size() const noexcept
	{
		// This is also used by the UI: independently sampled indices are only
		// telemetry, so clamp transient inconsistencies rather than underflow.
		const auto read = read_.load(std::memory_order_acquire);
		const auto write = write_.load(std::memory_order_acquire);
		return static_cast<size_t>(std::min<uint64_t>(write >= read ? write - read : 0, capacity_));
	}
	bool write(const float* source, size_t frames) noexcept
	{
		const auto write = write_.load(std::memory_order_relaxed);
		const auto read = read_.load(std::memory_order_acquire);
		if (frames > capacity_ - (write - read)) return false;
		const size_t offset = static_cast<size_t>(write % capacity_);
		const size_t first = (std::min)(frames, capacity_ - offset);
		std::copy_n(source, first * 2, audio_.data() + offset * 2);
		std::copy_n(source + first * 2, (frames - first) * 2, audio_.data());
		write_.store(write + frames, std::memory_order_release);
		return true;
	}
	size_t read(float* target, size_t frames) noexcept
	{
		const auto read = read_.load(std::memory_order_relaxed);
		const auto write = write_.load(std::memory_order_acquire);
		const size_t count = static_cast<size_t>((std::min)(uint64_t{frames}, write - read));
		const size_t offset = static_cast<size_t>(read % capacity_);
		const size_t first = (std::min)(count, capacity_ - offset);
		std::copy_n(audio_.data() + offset * 2, first * 2, target);
		std::copy_n(audio_.data(), (count - first) * 2, target + first * 2);
		std::fill(target + count * 2, target + frames * 2, 0.0f);
		read_.store(read + count, std::memory_order_release);
		return count;
	}
};

} // namespace safsyn::detail

#ifdef _MSC_VER
#pragma warning(pop)
#endif
