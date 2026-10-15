// Compare the live pending-event staging containers at identical queue depths.
// Includes first-burst allocation; these are transport timings, not audio FPS.
#include "playback_queue.h"
#include <chrono>
#include <deque>
#include <iomanip>
#include <iostream>
#include <optional>

namespace
{
struct Event
{
	uint64_t frame;
	std::optional<uint64_t> tick;
	uint32_t message;
	bool master;
};

template<class Factory>
void measure(const char* name, size_t depth, Factory make_queue)
{
	constexpr uint64_t events = 4'000'000;
	std::vector<double> times;
	uint64_t checksum = 0;
	for (unsigned trial = 0; trial < 8; ++trial)
	{
		auto queue = make_queue();
		uint64_t pushed = 0, popped = 0;
		checksum = 0;
		const auto started = std::chrono::steady_clock::now();
		while (popped < events)
		{
			while (pushed < events && queue.size() < depth)
			{
				queue.push_back({pushed / 256, pushed / 16, static_cast<uint32_t>(pushed), false});
				++pushed;
			}
			const size_t batch = (std::min)(size_t{4096}, queue.size());
			for (size_t i = 0; i < batch; ++i)
			{
				const auto& event = queue.front();
				checksum += event.frame + event.tick.value_or(0) + event.message;
				queue.pop_front();
				++popped;
			}
		}
		const double elapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		if (trial) times.push_back(elapsed);
	}
	std::sort(times.begin(), times.end());
	std::cout << name << ',' << depth << ',' << events << ',' << std::fixed
		<< std::setprecision(3) << times[times.size() / 2] << ',' << checksum << '\n';
}
}

int main()
{
	std::cout << "container,depth,events,median_ms,checksum\n";
	for (size_t depth : {64, 4096, 262144})
	{
		measure("deque", depth, [] { return std::deque<Event>{}; });
		measure("pending_ring", depth, [=] { return safsyn::detail::PendingQueue<Event>{depth}; });
	}
}
