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

// Overload shedding for live note events. Under pressure it drops the quietest
// note-ons and swallows the note-off that would have released each dropped
// note, so the engine receives a thinner but consistent stream. Controllers,
// programs and bends always pass. Owned by the single queue consumer.
class NoteGate
{
	// Note-ons still pending on one key, oldest first, starting at the oldest
	// dropped one: 1 reached the engine, 0 was dropped. The engine releases its
	// newest held note first, so note-offs pair with these newest-first.
	struct Key
	{
		std::vector<uint8_t> admitted;
		uint32_t dropped = 0;
	};
	std::vector<Key> keys_ = std::vector<Key>(16 * 128);
	float pressure_ = 0.0f;
	float dither_ = 0.0f;
	uint64_t shed_ = 0;
public:
	// Velocity range over which notes thin out gradually around the threshold.
	static constexpr float velocity_spread = 32.0f;

	// Zero admits every note and one admits none.
	void set_pressure(float pressure) noexcept
	{
		pressure_ = std::clamp(pressure, 0.0f, 1.0f);
	}
	uint64_t shed_notes() const noexcept { return shed_; }
	void clear() noexcept
	{
		for (auto& key : keys_) { key.admitted.clear(); key.dropped = 0; }
	}
	// False means the message must not reach the engine.
	bool admit(uint32_t message)
	{
		const uint32_t command = message & 0xf0, channel = message & 0x0f;
		const uint32_t data1 = (message >> 8) & 0x7f, data2 = (message >> 16) & 0x7f;
		if (command == 0x90 && data2 != 0)
		{
			auto& key = keys_[channel * 128 + data1];
			bool keep = true;
			if (pressure_ > 0.0f)
			{
				// Louder notes pass first. The golden-ratio sequence spreads the
				// survivors of one velocity evenly instead of cutting them at once.
				dither_ += 0.61803398875f;
				if (dither_ >= 1.0f) dither_ -= 1.0f;
				keep = static_cast<float>(data2) + velocity_spread * dither_ >
					pressure_ * (127.0f + velocity_spread);
			}
			if (keep)
			{
				if (key.dropped != 0) key.admitted.push_back(1);
				return true;
			}
			key.admitted.push_back(0);
			++key.dropped;
			++shed_;
			return false;
		}
		if (command == 0x80 || command == 0x90)
		{
			auto& key = keys_[channel * 128 + data1];
			if (key.dropped == 0) return true;
			const bool reached_engine = key.admitted.back() != 0;
			key.admitted.pop_back();
			if (reached_engine) return true;
			--key.dropped;
			return false;
		}
		// The engine forgets its held notes here, so later note-offs are its own.
		if (command == 0xb0 && (data1 == 120 || data1 >= 123))
			for (size_t note = 0; note < 128; ++note)
			{
				auto& key = keys_[channel * 128 + note];
				key.admitted.clear();
				key.dropped = 0;
			}
		return true;
	}
};

} // namespace safsyn::detail

#ifdef _MSC_VER
#pragma warning(pop)
#endif
