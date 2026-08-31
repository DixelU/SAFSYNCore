#include "windows_synth.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace
{
enum Control
{
	Bank = 100, BrowseBank, Midi, BrowseMidi, Output, Input, Refresh,
	Threads, Cohorts, Buffer, Block, Rate, InitialBank, Program, Gain,
	Phase, Strength, Seed, Pool, Continuous, Correlation, Attack,
	Limiter = 122,
	Live, Play, Stop, Test, Metrics, BlackMidiPreset, CacheLimit
};
struct App
{
	HWND window{};
	HFONT font{};
	safsyn::WindowsSynth synth;
	std::vector<safsyn::AudioOutputDevice> outputs;
	std::vector<safsyn::MidiInputDevice> inputs;
	std::vector<HWND> configuration;
	bool was_running = false, closing = false, file_mode = false, test_chord_active = false;
	uint32_t sample_rate = 48000;
};
HWND control(App& app, const wchar_t* type, const wchar_t* text, int id,
	int x, int y, int width, int height, DWORD style = 0, bool config = true)
{
	const auto handle = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
		type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, app.window,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
	SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
	if (config && id) app.configuration.push_back(handle);
	return handle;
}
void label(App& app, const wchar_t* text, int x, int y, int width = 140)
{ control(app, L"STATIC", text, 0, x, y + 4, width, 23); }
void edit(App& app, int id, const wchar_t* text, int x, int y, int width)
{ control(app, L"EDIT", text, id, x, y, width, 26, WS_TABSTOP | ES_AUTOHSCROLL); }
void button(App& app, int id, const wchar_t* text, int x, int y, int width, bool config = true)
{ control(app, L"BUTTON", text, id, x, y, width, 28, WS_TABSTOP | BS_PUSHBUTTON, config); }
HWND combo(App& app, int id, int x, int y, int width)
{ return control(app, L"COMBOBOX", L"", id, x, y, width, 220, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL); }
void add_item(HWND control, const std::wstring& text) { SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())); }
std::wstring text(HWND window, int id)
{
	const HWND field = GetDlgItem(window, id);
	std::wstring result(static_cast<size_t>(GetWindowTextLengthW(field)) + 1, L'\0');
	GetWindowTextW(field, result.data(), static_cast<int>(result.size())); result.pop_back();
	return result;
}
int selected(HWND window, int id) { return static_cast<int>(SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0)); }
bool checked(HWND window, int id) { return SendDlgItemMessageW(window, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
double number(HWND window, int id, double low, double high, const char* name)
{
	const auto value = text(window, id); size_t end = 0; double result = 0;
	try { result = std::stod(value, &end); } catch (...) { throw std::runtime_error(std::string("Invalid ") + name); }
	if (end != value.size() || !std::isfinite(result) || result < low || result > high)
		throw std::runtime_error(std::string("Out-of-range ") + name);
	return result;
}
uint32_t integer(HWND window, int id, uint32_t low, uint32_t high, const char* name)
{
	const double value = number(window, id, low, high, name);
	if (std::floor(value) != value) throw std::runtime_error(std::string("Expected integer ") + name);
	return static_cast<uint32_t>(value);
}
struct PhaseControls { bool strength, seed, pool, continuous, correlation, attack, cache; };
PhaseControls phase_controls(HWND window)
{
	const auto mode = static_cast<safsyn::PhaseMode>(selected(window, Phase));
	const bool active = mode != safsyn::PhaseMode::Coherent;
	const bool transform = active && mode != safsyn::PhaseMode::RandomPolarity;
	const bool analytic = mode == safsyn::PhaseMode::Analytic;
	return {transform, active, transform && !(analytic && checked(window, Continuous)),
		analytic, mode == safsyn::PhaseMode::SmoothField, active, transform};
}
void enable_phase_controls(App& app, bool running)
{
	const auto c = phase_controls(app.window);
	for (const auto [id, enabled] : {std::pair{Strength, c.strength}, {Seed, c.seed}, {Pool, c.pool},
		{Continuous, c.continuous}, {Correlation, c.correlation}, {Attack, c.attack}, {CacheLimit, c.cache}})
		EnableWindow(GetDlgItem(app.window, id), !running && enabled);
}
void enable_controls(App& app, bool running)
{
	for (auto field : app.configuration) EnableWindow(field, !running);
	enable_phase_controls(app, running);
	EnableWindow(GetDlgItem(app.window, Stop), running);
	EnableWindow(GetDlgItem(app.window, Test), FALSE); // Enabled after preparation/buffering.
}
void refresh_devices(App& app)
{
	app.outputs = safsyn::audio_output_devices(); app.inputs = safsyn::midi_input_devices();
	const HWND output = GetDlgItem(app.window, Output), input = GetDlgItem(app.window, Input);
	SendMessageW(output, CB_RESETCONTENT, 0, 0); SendMessageW(input, CB_RESETCONTENT, 0, 0);
	for (const auto& device : app.outputs) add_item(output, device.name);
	add_item(input, L"None (test notes / MIDI file only)");
	for (const auto& device : app.inputs) add_item(input, device.name);
	SendMessageW(output, CB_SETCURSEL, 0, 0); SendMessageW(input, CB_SETCURSEL, 0, 0);
}
std::wstring browse(HWND window, bool bank)
{
	std::wstring path(32768, L'\0');
	OPENFILENAMEW dialog{sizeof(dialog)};
	dialog.hwndOwner = window; dialog.lpstrFile = path.data(); dialog.nMaxFile = DWORD(path.size());
	dialog.lpstrFilter = bank ? L"Sound banks\0*.sf2;*.sfz\0\0" : L"Standard MIDI files\0*.mid;*.midi\0\0";
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&dialog)) return {};
	path.resize(wcslen(path.c_str())); return path;
}
void start(App& app, bool file)
{
	safsyn::WindowsSynthOptions request;
	request.bank_path = text(app.window, Bank);
	if (file)
	{
		request.midi_path = text(app.window, Midi);
		if (request.midi_path.empty()) throw std::runtime_error("Choose a MIDI file first.");
	}
	const int output = selected(app.window, Output), input = selected(app.window, Input);
	if (output < 0 || static_cast<size_t>(output) >= app.outputs.size()) throw std::runtime_error("Choose an audio output.");
	request.output_device = app.outputs[output].id;
	if (!file && input > 0) request.midi_input = static_cast<int>(app.inputs.at(input - 1).id);
	auto& options = request.playback;
	options.sample_rate = integer(app.window, Rate, 8000, 192000, "sample rate");
	options.block_frames = integer(app.window, Block, 16, 8192, "block frames");
	options.buffer_frames = static_cast<uint32_t>(number(app.window, Buffer, 10, 30000, "buffer milliseconds") * options.sample_rate / 1000);
	if (options.buffer_frames < options.block_frames * 2) throw std::runtime_error("The buffer must hold at least two render blocks.");
	options.render_threads = integer(app.window, Threads, 0, 64, "render threads");
	options.maximum_cohorts = integer(app.window, Cohorts, 1, 1048576, "cohort ceiling");
	options.initial_bank = static_cast<uint16_t>(integer(app.window, InitialBank, 0, 16383, "bank number"));
	options.initial_program = static_cast<uint8_t>(integer(app.window, Program, 0, 127, "program number"));
	options.mastering.output_gain_db = number(app.window, Gain, -120, 24, "gain dB");
	options.mastering.limiter_enabled = checked(app.window, Limiter);
	const int mode = selected(app.window, Phase);
	if (mode < 0 || mode > 4) throw std::runtime_error("Choose a phase mode.");
	options.phase.mode = static_cast<safsyn::PhaseMode>(mode);
	const auto c = phase_controls(app.window);
	// Inactive fields keep their text, but are never parsed or validated.
	if (c.strength) options.phase.strength = static_cast<float>(number(app.window, Strength, 0, 1, "phase strength"));
	if (c.pool) options.phase.pool_size = integer(app.window, Pool, 1, 64, "phase pool size");
	if (c.continuous) options.phase.continuous = checked(app.window, Continuous);
	if (c.correlation) options.phase.correlation_hz = static_cast<float>(number(app.window, Correlation, 0.001, 1000000, "correlation Hz"));
	if (c.attack) options.phase.preserve_attack_ms = static_cast<float>(number(app.window, Attack, 0, 10000, "preserved attack ms"));
	if (c.cache) options.maximum_phase_cache_bytes = uint64_t{integer(app.window, CacheLimit, 1, 1048576, "phase cache MiB")} * 1048576;
	if (c.seed)
	{
		const auto value = text(app.window, Seed); size_t end = 0;
		if (value.empty() || value.find(L'-') != std::wstring::npos) throw std::runtime_error("Invalid phase seed");
		options.phase.seed = std::stoull(value, &end);
		if (end != value.size()) throw std::runtime_error("Invalid phase seed");
	}
	app.sample_rate = options.sample_rate;
	app.file_mode = file;
	KillTimer(app.window, 2); app.test_chord_active = false;
	app.synth.start(request); app.was_running = true;
	enable_controls(app, true);
}
void create_controls(App& app)
{
	app.font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
	label(app, L"SAFSYN  |  Live synthesizer & black MIDI player", 20, 14, 750);
	label(app, L"Sound bank", 20, 52); edit(app, Bank, L"", 160, 52, 510); button(app, BrowseBank, L"Browse...", 680, 52, 100);
	label(app, L"Leave blank for the built-in sine instrument. SF2 bank/program numbers below are zero-based.", 160, 80, 620);
	label(app, L"MIDI file", 20, 112); edit(app, Midi, L"", 160, 112, 510); button(app, BrowseMidi, L"Browse...", 680, 112, 100);
	label(app, L"Audio output", 20, 153); combo(app, Output, 160, 153, 510); button(app, Refresh, L"Refresh", 680, 153, 100);
	label(app, L"Live MIDI input", 20, 192); combo(app, Input, 160, 192, 620);
	label(app, L"Render threads", 20, 242); edit(app, Threads, L"0", 160, 242, 80);
	label(app, L"0 = auto", 252, 242, 105);
	label(app, L"Cohort ceiling", 400, 242); edit(app, Cohorts, L"4096", 550, 242, 100);
	label(app, L"Buffer (ms)", 20, 280); edit(app, Buffer, L"100", 160, 280, 80);
	label(app, L"Block frames", 290, 280); edit(app, Block, L"256", 410, 280, 80);
	label(app, L"Sample rate", 530, 280); edit(app, Rate, L"48000", 660, 280, 120);
	label(app, L"Bank number", 20, 318); edit(app, InitialBank, L"0", 160, 318, 80);
	label(app, L"Program", 290, 318); edit(app, Program, L"0", 410, 318, 80);
	label(app, L"Output gain (dB)", 530, 318); edit(app, Gain, L"-12", 660, 318, 120);
	label(app, L"Phase mode", 20, 356);
	const auto phase = combo(app, Phase, 160, 356, 290);
	for (const auto name : {L"Coherent (off)", L"Random polarity", L"Analytic rotation", L"Smooth phase field", L"Independent FFT bins"}) add_item(phase, name);
	SendMessageW(phase, CB_SETCURSEL, 0, 0);
	control(app, L"BUTTON", L"Limiter (-1 dB ceiling)", Limiter, 470, 356, 290, 28, WS_TABSTOP | BS_AUTOCHECKBOX);
	SendDlgItemMessageW(app.window, Limiter, BM_SETCHECK, BST_CHECKED, 0);
	label(app, L"Strength", 20, 394, 75); edit(app, Strength, L"1", 100, 394, 60);
	label(app, L"Pool", 195, 394, 55); edit(app, Pool, L"8", 250, 394, 65);
	control(app, L"BUTTON", L"Continuous analytic", Continuous, 340, 394, 200, 28, WS_TABSTOP | BS_AUTOCHECKBOX);
	label(app, L"Cache MiB", 550, 394, 105); edit(app, CacheLimit, L"2048", 660, 394, 120);
	label(app, L"Seed", 20, 432, 75); edit(app, Seed, L"0", 100, 432, 150);
	label(app, L"Correlation Hz", 270, 432, 120); edit(app, Correlation, L"250", 390, 432, 100);
	label(app, L"Keep attack ms", 530, 432, 125); edit(app, Attack, L"0", 660, 432, 120);
	button(app, BlackMidiPreset, L"Black MIDI preset", 20, 474, 190);
	label(app, L"512 cohorts, 4 threads, 10 s buffer. Large buffers add latency.", 230, 474, 550);
	label(app, L"Stop to change phase. All bank samples are prepared before playback; FFT working memory is extra.", 20, 512, 770);
	button(app, Live, L"Start live synth", 20, 548, 170);
	button(app, Play, L"Play MIDI file", 205, 548, 170);
	button(app, Test, L"Test chord", 390, 548, 170, false);
	button(app, Stop, L"Panic / stop", 575, 548, 205, false);
	control(app, L"EDIT", L"Stopped", Metrics, 20, 590, 760, 155,
		ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, false);
	refresh_devices(app); enable_controls(app, false);
}
void update(App& app)
{
	const auto state = app.synth.stats();
	const auto& s = state.playback;
	std::wostringstream output;
	output << state.status << L"\r\n" << std::fixed << std::setprecision(2)
		<< L"Played: " << double(s.consumed_frames) / app.sample_rate << L" s    Buffered: "
		<< 1000.0 * s.buffered_frames / app.sample_rate << L" ms    Render threads: " << s.render_threads
		<< L"    Block load: " << s.render_load * 100 << L"%\r\n"
		<< L"Voices: " << s.active_voices << L"    Cohorts: " << s.active_cohorts << L"    Steals: "
		<< s.engine.cohort_capacity_steals << L"    Events: " << s.scheduled_events << L"\r\n"
		<< L"Underruns: " << s.underruns << L" (" << s.underrun_frames << L" frames)    MIDI rejected: "
		<< s.rejected_midi_events << L"    Recoveries: " << s.midi_recoveries << L"\r\n"
		<< L"Raw peak: " << s.raw_peak << L"    Device buffer: " << state.device_buffer_frames
		<< L" frames    Empty device buffers: " << state.empty_device_buffers;
	output << L"\r\nPhase preparation: " << s.preparation.completed << L"/" << s.preparation.total
		<< L"    Cache: " << double(s.phase.cache_bytes) / 1048576 << L" / "
		<< double(s.preparation.total_cache_bytes) / 1048576 << L" MiB    Prep: " << s.preparation_ms / 1000 << L" s";
	if (!state.error.empty()) output << L"\r\n" << std::wstring(state.error.begin(), state.error.end());
	SetDlgItemTextW(app.window, Metrics, output.str().c_str());
	if (app.was_running && !state.running) { app.was_running = false; enable_controls(app, false); }
	EnableWindow(GetDlgItem(app.window, Test), state.running && s.ready && !s.preparing && !app.file_mode);
	if (app.closing && !state.running) DestroyWindow(app.window);
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE)
	{
		app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
		app->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
	}
	if (!app) return DefWindowProcW(window, message, wparam, lparam);
	try
	{
		switch (message)
		{
		case WM_CREATE: create_controls(*app); DragAcceptFiles(window, TRUE); SetTimer(window, 1, 100, nullptr); return 0;
		case WM_COMMAND:
			switch (LOWORD(wparam))
			{
			case BrowseBank: case BrowseMidi:
			{
				const bool bank = LOWORD(wparam) == BrowseBank; const auto path = browse(window, bank);
				if (!path.empty()) SetDlgItemTextW(window, bank ? Bank : Midi, path.c_str()); return 0;
			}
			case Refresh: refresh_devices(*app); return 0;
			case Phase: case Continuous: enable_phase_controls(*app, app->was_running); return 0;
			case BlackMidiPreset:
				SetDlgItemTextW(window, Cohorts, L"512"); SetDlgItemTextW(window, Threads, L"4");
				SetDlgItemTextW(window, Buffer, L"10000"); return 0;
			case Live: start(*app, false); return 0;
			case Play: start(*app, true); return 0;
			case Stop: app->synth.request_stop(); return 0;
			case Test:
				if (app->test_chord_active)
					for (uint32_t note : {60, 64, 67}) app->synth.send_short_message(0x80 | (note << 8));
				for (uint32_t note : {60, 64, 67}) app->synth.send_short_message(0x00500090 | (note << 8));
				app->test_chord_active = true;
				SetTimer(window, 2, 500, nullptr); return 0;
			}
			break;
		case WM_TIMER:
			if (wparam == 1) update(*app);
			else if (wparam == 2)
			{
				for (uint32_t note : {60, 64, 67}) app->synth.send_short_message(0x80 | (note << 8));
				KillTimer(window, 2);
				app->test_chord_active = false;
			}
			return 0;
		case WM_DROPFILES:
		{
			const auto drop = reinterpret_cast<HDROP>(wparam);
			if (!app->was_running)
				for (UINT i = 0; i < DragQueryFileW(drop, 0xffffffff, nullptr, 0); ++i)
				{
					std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
					DragQueryFileW(drop, i, path.data(), UINT(path.size())); path.pop_back();
					const auto ext = std::filesystem::path(path).extension().wstring();
					if (_wcsicmp(ext.c_str(), L".sf2") == 0 || _wcsicmp(ext.c_str(), L".sfz") == 0)
						SetDlgItemTextW(window, Bank, path.c_str());
					else if (_wcsicmp(ext.c_str(), L".mid") == 0 || _wcsicmp(ext.c_str(), L".midi") == 0)
						SetDlgItemTextW(window, Midi, path.c_str());
				}
			DragFinish(drop); return 0;
		}
		case WM_CLOSE:
			app->closing = true; app->synth.request_stop();
			SetWindowTextW(window, L"SAFSYN - stopping...");
			if (!app->synth.stats().running) DestroyWindow(window); return 0;
		case WM_DESTROY: KillTimer(window, 1); KillTimer(window, 2); PostQuitMessage(0); return 0;
		}
	}
	catch (const std::exception& error)
	{
		MessageBoxA(window, error.what(), "SAFSYN", MB_OK | MB_ICONERROR);
		if (message == WM_CREATE) return -1;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
	SetProcessDPIAware();
	INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
	try
	{
		App app;
		WNDCLASSW type{}; type.lpfnWndProc = window_proc; type.hInstance = instance;
		type.lpszClassName = L"SAFSYN.LiveSynth"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		RegisterClassW(&type);
		RECT bounds{0, 0, 802, 768};
		const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
		AdjustWindowRect(&bounds, style, FALSE);
		const HWND window = CreateWindowW(type.lpszClassName, L"SAFSYN Synth", style,
			CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
			nullptr, nullptr, instance, &app);
		if (!window) return 1;
		ShowWindow(window, show);
		MSG message{};
		while (GetMessageW(&message, nullptr, 0, 0) > 0)
			if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
		app.synth.stop(); DeleteObject(app.font);
		return static_cast<int>(message.wParam);
	}
	catch (const std::exception& error) { MessageBoxA(nullptr, error.what(), "SAFSYN", MB_OK | MB_ICONERROR); return 1; }
}
