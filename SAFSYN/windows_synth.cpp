#define NOMINMAX
#include "windows_synth.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace safsyn
{
namespace
{
using Microsoft::WRL::ComPtr;
void require(HRESULT result, const char* operation)
{
	if (FAILED(result))
	{
		std::ostringstream text; text << operation << " failed (0x" << std::hex << uint32_t(result) << ')';
		throw std::runtime_error(text.str());
	}
}
void require_midi(MMRESULT result, const char* operation)
{
	if (result != MMSYSERR_NOERROR)
	{
		char description[MAXERRORLENGTH]{};
		midiInGetErrorTextA(result, description, MAXERRORLENGTH);
		throw std::runtime_error(std::string(operation) + ": " + description);
	}
}
struct ComApartment
{
	ComApartment() { require(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization"); }
	~ComApartment() { CoUninitialize(); }
};
struct EventHandle
{
	HANDLE value;
	explicit EventHandle(bool manual = false) : value(CreateEventW(nullptr, manual, FALSE, nullptr))
	{ if (!value) throw std::runtime_error("could not create event"); }
	~EventHandle() { CloseHandle(value); }
	EventHandle(const EventHandle&) = delete;
	EventHandle& operator=(const EventHandle&) = delete;
};
struct Mmcss
{
	DWORD index = 0;
	HANDLE handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
	~Mmcss() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};
struct AudioStop
{
	IAudioClient* client;
	~AudioStop() { client->Stop(); }
};

std::shared_ptr<Soundfont> load_bank(const std::wstring& path)
{
	auto bank = std::make_shared<Soundfont>();
	if (path.empty())
	{
		constexpr uint32_t length = 2048;
		bank->sfz_pcm.emplace_back(length);
		for (size_t i = 0; i < length; ++i)
			bank->sfz_pcm[0][i] = static_cast<int16_t>(std::sin(i * 6.283185307179586 / length) * 12000);
		SampleRegion region;
		region.pcm = bank->sfz_pcm[0].data(); region.pcm_len = length;
		region.root_key = 69; region.sample_rate = length * 440;
		region.loop_mode = LoopMode::Forward; region.loop_end = length;
		region.attack = 0.005f; region.release = 0.1f;
		bank->regions.push_back(region);
		return bank;
	}
	// Existing loaders accept native narrow paths. Fail on an unrepresentable
	// path rather than substitute '?' and accidentally load a different file.
	BOOL substituted = FALSE;
	const int count = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), -1,
		nullptr, 0, nullptr, &substituted);
	std::string native(static_cast<size_t>(count), '\0');
	if (!count || !WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), -1,
		native.data(), count, nullptr, &substituted) || substituted)
		throw std::runtime_error("bank path is not supported by the loader; use a path in the Windows system code page");
	const auto extension = std::filesystem::path(path).extension().wstring();
	const bool ok = _wcsicmp(extension.c_str(), L".sfz") == 0 ? load_sfz(native.c_str(), *bank) :
		_wcsicmp(extension.c_str(), L".sf2") == 0 && load_sf2(native.c_str(), *bank);
	if (!ok || bank->regions.empty()) throw std::runtime_error("could not load SF2/SFZ sound bank");
	return bank;
}

std::shared_ptr<SmfFile> load_midi(const std::wstring& path)
{
	// Load bytes through a wide filesystem path; parsing remains the portable,
	// streaming scheduler. There is no full analysis pass before playback.
	std::ifstream source(std::filesystem::path(path), std::ios::binary | std::ios::ate);
	if (!source) throw std::runtime_error("could not open MIDI file");
	const auto length = source.tellg();
	if (length < 0 || static_cast<uint64_t>(length) > SIZE_MAX)
		throw std::runtime_error("invalid MIDI file size");
	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	source.seekg(0);
	if (!bytes.empty() && !source.read(reinterpret_cast<char*>(bytes.data()), length))
		throw std::runtime_error("could not read MIDI file");
	auto file = std::make_shared<SmfFile>();
	if (!file->load_bytes(std::move(bytes)))
		throw std::runtime_error(file->diagnostics().empty() ? "invalid MIDI file" : file->diagnostics().front().message);
	return file;
}

class MidiInput
{
	struct Slot
	{
		std::array<char, 1024> bytes{};
		MIDIHDR header{};
		std::atomic<bool> returned{false};
		bool prepared = false;
	};
	BufferedSynth& synth_;
	std::array<Slot, 4> slots_;
	EventHandle stop_{true}, returned_;
	std::thread service_;
	std::atomic<MMRESULT> error_{MMSYSERR_NOERROR};
	std::array<uint8_t, 8> sysex_{};
	size_t sysex_size_ = 0;
	bool in_sysex_ = false, oversized_ = false;

	void sysex_byte(uint8_t byte) noexcept
	{
		if (byte >= 0xf8) return;
		if (byte == 0xf0) { in_sysex_ = true; oversized_ = false; sysex_size_ = 0; }
		if (!in_sysex_) return;
		if (sysex_size_ < sysex_.size()) sysex_[sysex_size_++] = byte;
		else oversized_ = true;
		if (byte == 0xf7)
		{
			if (!oversized_ && sysex_size_ == 8 && sysex_[1] == 0x7f &&
				sysex_[2] < 128 && sysex_[3] == 4 && sysex_[4] == 1 &&
				sysex_[5] < 128 && sysex_[6] < 128)
				synth_.enqueue_master_volume(static_cast<uint16_t>(sysex_[5] | (sysex_[6] << 7)));
			in_sysex_ = false;
		}
	}
	static void CALLBACK callback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR data, DWORD_PTR) noexcept
	{
		auto& self = *reinterpret_cast<MidiInput*>(instance);
		if (message == MIM_DATA || message == MIM_MOREDATA)
			self.synth_.enqueue_short_message(static_cast<uint32_t>(data));
		else if (message == MIM_ERROR) self.synth_.report_input_loss();
		else if (message == MIM_LONGDATA || message == MIM_LONGERROR)
		{
			auto& header = *reinterpret_cast<MIDIHDR*>(data);
			if (message == MIM_LONGERROR) { self.in_sysex_ = false; self.synth_.report_input_loss(); }
			else for (DWORD i = 0; i < (std::min)(header.dwBytesRecorded, header.dwBufferLength); ++i)
				self.sysex_byte(static_cast<uint8_t>(header.lpData[i]));
			self.slots_[header.dwUser].returned.store(true, std::memory_order_release);
			SetEvent(self.returned_.value); // No multimedia calls from the callback.
		}
	}
public:
	MidiInput(UINT device, BufferedSynth& synth) : synth_(synth)
	{
		std::promise<void> initialized;
		auto result = initialized.get_future();
		service_ = std::thread([this, device, initialized = std::move(initialized)]() mutable {
			HMIDIIN input = nullptr;
			bool announced = false;
			try
			{
				require_midi(midiInOpen(&input, device, reinterpret_cast<DWORD_PTR>(&callback),
					reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION | MIDI_IO_STATUS), "MIDI input open");
				for (size_t i = 0; i < slots_.size(); ++i)
				{
					auto& slot = slots_[i];
					slot.header.lpData = slot.bytes.data(); slot.header.dwBufferLength = DWORD(slot.bytes.size());
					slot.header.dwUser = i;
					require_midi(midiInPrepareHeader(input, &slot.header, sizeof(MIDIHDR)), "MIDI prepare");
					slot.prepared = true;
					require_midi(midiInAddBuffer(input, &slot.header, sizeof(MIDIHDR)), "MIDI buffer");
				}
				require_midi(midiInStart(input), "MIDI input start");
				initialized.set_value(); announced = true;
				const HANDLE waits[]{stop_.value, returned_.value};
				for (;;)
				{
					const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
					if (wait == WAIT_OBJECT_0) break;
					if (wait != WAIT_OBJECT_0 + 1) throw std::runtime_error("MIDI input event wait failed");
					for (auto& slot : slots_)
						if (slot.returned.exchange(false, std::memory_order_acq_rel))
						{
							slot.header.dwBytesRecorded = 0;
							require_midi(midiInAddBuffer(input, &slot.header, sizeof(MIDIHDR)), "MIDI requeue");
						}
				}
			}
			catch (...)
			{
				if (!announced) initialized.set_exception(std::current_exception());
				else { error_.store(MMSYSERR_ERROR); synth_.report_input_loss(); }
			}
			if (input)
			{
				midiInStop(input); midiInReset(input);
				for (auto& slot : slots_) if (slot.prepared)
					midiInUnprepareHeader(input, &slot.header, sizeof(MIDIHDR));
				midiInClose(input);
			}
		});
		try { result.get(); }
		catch (...) { SetEvent(stop_.value); service_.join(); throw; }
	}
	~MidiInput() { SetEvent(stop_.value); if (service_.joinable()) service_.join(); }
	MMRESULT error() const noexcept { return error_.load(); }
};
}

std::vector<AudioOutputDevice> audio_output_devices()
{
	ComApartment apartment;
	std::vector<AudioOutputDevice> result{{L"", L"Default Windows output"}};
	ComPtr<IMMDeviceEnumerator> enumerator;
	require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Audio device enumeration");
	ComPtr<IMMDeviceCollection> collection;
	require(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "Audio endpoints");
	UINT count = 0; require(collection->GetCount(&count), "Audio endpoint count");
	for (UINT i = 0; i < count; ++i)
	{
		ComPtr<IMMDevice> device; require(collection->Item(i, &device), "Audio endpoint");
		LPWSTR id = nullptr; require(device->GetId(&id), "Audio endpoint ID");
		AudioOutputDevice entry;
		entry.id = id; CoTaskMemFree(id); entry.name = entry.id;
		ComPtr<IPropertyStore> properties;
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
		{
			PROPVARIANT name{};
			if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
				entry.name = name.pwszVal;
			PropVariantClear(&name);
		}
		result.push_back(std::move(entry));
	}
	return result;
}
std::vector<MidiInputDevice> midi_input_devices()
{
	std::vector<MidiInputDevice> result;
	for (UINT i = 0; i < midiInGetNumDevs(); ++i)
	{
		MIDIINCAPSW caps{};
		if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			result.push_back({i, caps.szPname});
	}
	return result;
}

struct WindowsSynth::Impl
{
	// start(), request_stop(), and stop() are public control-plane operations.
	// Serializing them prevents a stop that arrives during start from being
	// cleared by the new session and then joining that uncancelled session.
	std::mutex lifecycle_mutex;
	mutable std::mutex mutex;
	std::shared_ptr<BufferedSynth> synth;
	std::thread delivery;
	EventHandle stop_event{true};
	std::atomic<bool> running{false}, cancelled{false};
	std::atomic<uint32_t> device_frames{0};
	std::atomic<uint64_t> empty_buffers{0};
	std::wstring status = L"Stopped";
	std::string error;
	void set_status(const wchar_t* text) { std::lock_guard lock(mutex); status = text; }
	void request_stop_locked() noexcept
	{
		cancelled.store(true);
		SetEvent(stop_event.value);
		std::lock_guard lock(mutex);
		if (synth)
			synth->request_stop();
	}
	void run(WindowsSynthOptions options) noexcept
	{
		try
		{
			set_status(L"Loading sound bank...");
			auto bank = load_bank(options.bank_path);
			if (cancelled.load()) { set_status(L"Stopped"); running.store(false); return; }
			std::shared_ptr<SmfFile> midi;
			if (!options.midi_path.empty()) { set_status(L"Loading MIDI file..."); midi = load_midi(options.midi_path); }
			if (cancelled.load()) { set_status(L"Stopped"); running.store(false); return; }
			auto playback = std::make_shared<BufferedSynth>(bank, options.playback, midi);
			{ std::lock_guard lock(mutex); synth = playback; }
			ComApartment apartment;
			ComPtr<IMMDeviceEnumerator> enumerator;
			require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "WASAPI enumerator");
			ComPtr<IMMDevice> device;
			if (options.output_device.empty())
				require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device), "Default audio output");
			else require(enumerator->GetDevice(options.output_device.c_str(), &device), "Selected audio output");
			ComPtr<IAudioClient> client;
			require(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
				reinterpret_cast<void**>(client.GetAddressOf())), "WASAPI activation");
			WAVEFORMATEX format{};
			format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; format.nChannels = 2;
			format.nSamplesPerSec = options.playback.sample_rate; format.wBitsPerSample = 32;
			format.nBlockAlign = 8; format.nAvgBytesPerSec = format.nSamplesPerSec * 8;
			EventHandle audio_event;
			require(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				200000, 0, &format, nullptr), "WASAPI shared float stream");
			require(client->SetEventHandle(audio_event.value), "WASAPI event");
			UINT32 capacity = 0; require(client->GetBufferSize(&capacity), "WASAPI buffer size");
			device_frames.store(capacity);
			ComPtr<IAudioRenderClient> render;
			require(client->GetService(IID_PPV_ARGS(&render)), "WASAPI render service");
			if (options.test_note && !midi) playback->enqueue_short_message(0x00644590);
			playback->start();
			std::unique_ptr<MidiInput> input;
			set_status(L"Buffering...");
			while (!playback->ready() && !cancelled.load()) WaitForSingleObject(stop_event.value, 5);
			if (!cancelled.load())
			{
				const auto initial = playback->stats();
				if (!initial.error.empty()) throw std::runtime_error(initial.error);
				// Do not accept live input during potentially lengthy phase preparation.
				if (!midi && options.midi_input >= 0)
					input = std::make_unique<MidiInput>(static_cast<UINT>(options.midi_input), *playback);
				Mmcss priority;
				BYTE* bytes = nullptr;
				require(render->GetBuffer(capacity, &bytes), "WASAPI prime buffer");
				playback->read_audio(reinterpret_cast<float*>(bytes), capacity);
				require(render->ReleaseBuffer(capacity, options.mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0), "WASAPI prime release");
				require(client->Start(), "WASAPI start");
				AudioStop stop_client{client.Get()};
				set_status(midi ? L"Playing MIDI file" : L"Live MIDI / test keyboard");
				const auto started = std::chrono::steady_clock::now();
				bool released_test_note = false;
				const HANDLE waits[]{stop_event.value, audio_event.value};
				while (!cancelled.load())
				{
					const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 2000);
					if (wait == WAIT_OBJECT_0) break;
					if (wait != WAIT_OBJECT_0 + 1) throw std::runtime_error("audio endpoint stopped delivering events; restart the synth");
					const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
					if (options.maximum_seconds > 0 && elapsed >= options.maximum_seconds) break;
					if (options.test_note && !released_test_note && elapsed >= 0.4)
					{ playback->enqueue_short_message(0x00004580); released_test_note = true; }
					if (input && input->error() != MMSYSERR_NOERROR) throw std::runtime_error("MIDI input service failed; restart the synth");
					UINT32 padding = 0; require(client->GetCurrentPadding(&padding), "WASAPI padding");
					if (playback->drained()) { if (padding == 0) break; else continue; }
					if (padding == 0) empty_buffers.fetch_add(1, std::memory_order_relaxed);
					const UINT32 available = capacity - padding;
					if (!available) continue;
					require(render->GetBuffer(available, &bytes), "WASAPI buffer");
					playback->read_audio(reinterpret_cast<float*>(bytes), available);
					require(render->ReleaseBuffer(available, options.mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0), "WASAPI release");
				}
			}
			input.reset();
			playback->stop();
			const auto final = playback->stats();
			if (!final.error.empty()) throw std::runtime_error(final.error);
			set_status(cancelled.load() ? L"Stopped" : L"Finished");
		}
		catch (const std::exception& exception)
		{
			std::shared_ptr<BufferedSynth> playback;
			{ std::lock_guard lock(mutex); playback = synth; error = exception.what(); status = L"Error"; }
			if (playback) playback->stop();
		}
		catch (...)
		{
			std::shared_ptr<BufferedSynth> playback;
			{ std::lock_guard lock(mutex); playback = synth; error = "unknown Windows synth failure"; status = L"Error"; }
			if (playback) playback->stop();
		}
		running.store(false, std::memory_order_release);
	}
};

WindowsSynth::WindowsSynth() : impl_(std::make_unique<Impl>()) {}
WindowsSynth::~WindowsSynth() { stop(); }
void WindowsSynth::start(const WindowsSynthOptions& options)
{
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	if (impl_->running.load()) throw std::logic_error("synth is already running");
	if (impl_->delivery.joinable()) impl_->delivery.join();
	{ std::lock_guard lock(impl_->mutex); impl_->synth.reset(); impl_->error.clear(); impl_->status = L"Starting..."; }
	impl_->device_frames.store(0); impl_->empty_buffers.store(0);
	impl_->cancelled.store(false); ResetEvent(impl_->stop_event.value);
	impl_->running.store(true);
	try { impl_->delivery = std::thread([this, options] { impl_->run(options); }); }
	catch (...) { impl_->running.store(false); throw; }
}
void WindowsSynth::request_stop() noexcept
{
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	impl_->request_stop_locked();
}
void WindowsSynth::stop() noexcept
{
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	impl_->request_stop_locked();
	if (impl_->delivery.joinable())
		impl_->delivery.join();
}
bool WindowsSynth::send_short_message(uint32_t message) noexcept
{
	std::lock_guard lock(impl_->mutex);
	return impl_->synth && impl_->synth->enqueue_short_message(message);
}
void WindowsSynth::panic() noexcept
{
	std::lock_guard lock(impl_->mutex);
	if (impl_->synth) impl_->synth->panic();
}
WindowsSynthStats WindowsSynth::stats() const
{
	WindowsSynthStats result;
	std::shared_ptr<BufferedSynth> playback;
	{ std::lock_guard lock(impl_->mutex); playback = impl_->synth; result.status = impl_->status; result.error = impl_->error; }
	if (playback) result.playback = playback->stats();
	result.running = impl_->running.load(std::memory_order_acquire);
	if (result.running && result.playback.preparing && result.error.empty()) result.status = L"Preparing phase cache...";
	result.device_buffer_frames = impl_->device_frames.load();
	result.empty_device_buffers = impl_->empty_buffers.load();
	return result;
}

} // namespace safsyn
