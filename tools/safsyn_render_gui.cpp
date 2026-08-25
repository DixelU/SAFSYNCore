#include "core.h"
#include "mastering.h"
#include "smf.h"
#include "smf_renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(linker, \
	"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
	"version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
	"language='*'\"")
#endif

namespace
{
constexpr wchar_t kWindowClass[] = L"SAFSYNRendererGui";
constexpr UINT kProgressMessage = WM_APP + 1;
constexpr UINT kDoneMessage = WM_APP + 2;
constexpr UINT kStageMessage = WM_APP + 3;

enum ControlId : int
{
	MidiPath = 100,
	BankPath,
	OutputPath,
	BrowseMidi,
	BrowseBank,
	BrowseOutput,
	SettingsTabs,
	SampleRate,
	RenderThreads,
	VoiceModel,
	VoiceCapacity,
	MaximumCohorts,
	InitialBank,
	InitialProgram,
	TailMode,
	TailSeconds,
	MaximumSeconds,
	BlockFrames,
	AllRegions,
	PhaseMode,
	PhaseStrength,
	PhasePool,
	PhaseContinuous,
	PhaseSeed,
	PhaseCorrelation,
	PhaseAttack,
	OutputGain,
	LimiterEnabled,
	LimiterCeiling,
	LimiterLookahead,
	LimiterRelease,
	RenderButton,
	CancelButton,
	ProgressBar,
	StatusTitle,
	StatusDetail,
	StatusMetrics,
};

struct RenderRequest
{
	std::wstring midi_path;
	std::wstring bank_path;
	std::wstring output_path;
	safsyn::SmfRenderOptions options;
};

struct ProgressUpdate
{
	safsyn::SmfRenderProgress progress;
	double elapsed_seconds = 0.0;
};

struct StageUpdate
{
	std::wstring title;
	std::wstring detail;
};

struct DoneUpdate
{
	bool success = false;
	bool cancelled = false;
	std::wstring error;
	safsyn::SmfRenderResult result;
	safsyn::SmfAnalysis analysis;
	double elapsed_seconds = 0.0;
	uint64_t output_bytes = 0;
};

struct CallbackContext
{
	HWND window = nullptr;
	std::atomic_bool* cancel = nullptr;
	std::chrono::steady_clock::time_point started;
	std::chrono::steady_clock::time_point last_posted{};
	safsyn::SmfRenderProgressStage last_stage = safsyn::SmfRenderProgressStage::Preparing;
	bool posted = false;
};

struct AppState
{
	HWND window = nullptr;
	HWND midi_path = nullptr;
	HWND bank_path = nullptr;
	HWND output_path = nullptr;
	HWND tabs = nullptr;
	HWND progress = nullptr;
	HWND status_title = nullptr;
	HWND status_detail = nullptr;
	HWND status_metrics = nullptr;
	HWND render_button = nullptr;
	HWND cancel_button = nullptr;
	HFONT font = nullptr;
	HFONT font_bold = nullptr;
	HFONT font_title = nullptr;
	HBRUSH background = nullptr;
	std::vector<HWND> page_controls[3];
	std::vector<HWND> input_controls;
	std::thread worker;
	std::atomic_bool cancel{false};
	bool running = false;
	bool closing = false;
};

AppState* state_from(HWND window)
{
	return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring control_text(HWND control)
{
	const int length = GetWindowTextLengthW(control);
	std::wstring text(static_cast<size_t>(length) + 1, L'\0');
	GetWindowTextW(control, text.data(), length + 1);
	text.resize(static_cast<size_t>(length));
	return text;
}

void set_text(HWND control, const std::wstring& text)
{
	SetWindowTextW(control, text.c_str());
}

std::wstring trim(std::wstring value)
{
	const auto not_space = [](wchar_t ch) { return !iswspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::wstring lowercase_extension(const std::wstring& path)
{
	std::wstring extension = std::filesystem::path(path).extension().wstring();
	std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
	return extension;
}

std::wstring widen(const std::string& value)
{
	if (value.empty()) return {};
	auto convert = [&](UINT code_page) -> std::wstring {
		const int count = MultiByteToWideChar(code_page, 0, value.data(),
			static_cast<int>(value.size()), nullptr, 0);
		if (count <= 0) return {};
		std::wstring result(static_cast<size_t>(count), L'\0');
		MultiByteToWideChar(code_page, 0, value.data(), static_cast<int>(value.size()),
			result.data(), count);
		return result;
	};
	std::wstring result = convert(CP_UTF8);
	return result.empty() ? convert(CP_ACP) : result;
}

std::wstring diagnostics_text(const std::vector<safsyn::SmfDiagnostic>& diagnostics)
{
	std::wostringstream text;
	for (const auto& diagnostic : diagnostics)
	{
		if (text.tellp() > 0) text << L"\n";
		text << (diagnostic.severity == safsyn::SmfDiagnosticSeverity::Error
			? L"Error" : L"Warning");
		if (diagnostic.track != UINT32_MAX)
			text << L" in track " << (diagnostic.track + 1);
		text << L": " << widen(diagnostic.message);
	}
	return text.str();
}

std::wstring format_duration(double seconds)
{
	if (!std::isfinite(seconds) || seconds < 0.0) return L"—";
	const uint64_t rounded = static_cast<uint64_t>(std::llround(seconds));
	const uint64_t hours = rounded / 3600;
	const uint64_t minutes = (rounded / 60) % 60;
	const uint64_t remaining = rounded % 60;
	wchar_t buffer[64] = {};
	if (hours != 0)
		swprintf_s(buffer, L"%llu:%02llu:%02llu", hours, minutes, remaining);
	else
		swprintf_s(buffer, L"%llu:%02llu", minutes, remaining);
	return buffer;
}

std::wstring format_count(uint64_t value)
{
	std::wstring digits = std::to_wstring(value);
	for (ptrdiff_t index = static_cast<ptrdiff_t>(digits.size()) - 3; index > 0; index -= 3)
		digits.insert(static_cast<size_t>(index), 1, L',');
	return digits;
}

std::wstring format_bytes(uint64_t bytes)
{
	const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
	double value = static_cast<double>(bytes);
	size_t unit = 0;
	while (value >= 1024.0 && unit + 1 < std::size(units))
	{
		value /= 1024.0;
		++unit;
	}
	wchar_t buffer[64] = {};
	if (unit == 0) swprintf_s(buffer, L"%llu %s", bytes, units[unit]);
	else swprintf_s(buffer, L"%.2f %s", value, units[unit]);
	return buffer;
}

std::wstring format_peak(float peak)
{
	if (peak <= 0.0f) return L"−∞ dBFS";
	wchar_t buffer[64] = {};
	swprintf_s(buffer, L"%.2f dBFS", 20.0 * std::log10(static_cast<double>(peak)));
	return buffer;
}

bool seconds_to_frames(double seconds, uint32_t sample_rate, uint64_t& frames)
{
	if (!std::isfinite(seconds) || seconds < 0.0) return false;
	const long double exact = static_cast<long double>(seconds) * sample_rate;
	if (exact > static_cast<long double>((std::numeric_limits<uint64_t>::max)()) - 0.5L)
		return false;
	frames = static_cast<uint64_t>(std::floor(exact + 0.5L));
	return true;
}

HWND create_control(AppState& state, const wchar_t* class_name, const wchar_t* text,
	DWORD style, DWORD extended_style, int id, int x, int y, int width, int height,
	HFONT font = nullptr)
{
	HWND control = CreateWindowExW(extended_style, class_name, text,
		style | WS_CHILD | WS_VISIBLE, x, y, width, height, state.window,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
	SendMessageW(control, WM_SETFONT,
		reinterpret_cast<WPARAM>(font ? font : state.font), TRUE);
	return control;
}

HWND add_page_label(AppState& state, int page, const wchar_t* text,
	int x, int y, int width, int height = 22)
{
	HWND control = create_control(state, L"STATIC", text, SS_LEFT, 0, 0,
		x, y, width, height);
	state.page_controls[page].push_back(control);
	return control;
}

HWND add_page_edit(AppState& state, int page, int id, const wchar_t* value,
	int x, int y, int width = 140)
{
	HWND control = create_control(state, L"EDIT", value,
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, id, x, y, width, 25);
	state.page_controls[page].push_back(control);
	state.input_controls.push_back(control);
	return control;
}

HWND add_page_combo(AppState& state, int page, int id,
	int x, int y, int width, const std::vector<const wchar_t*>& values, int selection)
{
	HWND control = create_control(state, WC_COMBOBOXW, L"",
		CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, id, x, y, width, 200);
	for (const wchar_t* value : values)
		SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
	SendMessageW(control, CB_SETCURSEL, selection, 0);
	state.page_controls[page].push_back(control);
	state.input_controls.push_back(control);
	return control;
}

HWND add_page_check(AppState& state, int page, int id, const wchar_t* text,
	int x, int y, int width, bool checked = false)
{
	HWND control = create_control(state, L"BUTTON", text,
		BS_AUTOCHECKBOX | WS_TABSTOP, 0, id, x, y, width, 25);
	SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
	state.page_controls[page].push_back(control);
	state.input_controls.push_back(control);
	return control;
}

void show_page(AppState& state, int page)
{
	for (int index = 0; index < 3; ++index)
		for (HWND control : state.page_controls[index])
			ShowWindow(control, index == page ? SW_SHOW : SW_HIDE);
}

void set_marquee(HWND progress, bool enabled)
{
	LONG_PTR style = GetWindowLongPtrW(progress, GWL_STYLE);
	const bool already_enabled = (style & PBS_MARQUEE) != 0;
	if (already_enabled == enabled) return;
	if (enabled) style |= PBS_MARQUEE;
	else style &= ~static_cast<LONG_PTR>(PBS_MARQUEE);
	SetWindowLongPtrW(progress, GWL_STYLE, style);
	SetWindowPos(progress, nullptr, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	SendMessageW(progress, PBM_SETMARQUEE, enabled, 30);
}

void set_running(AppState& state, bool running)
{
	state.running = running;
	for (HWND control : state.input_controls)
		EnableWindow(control, !running);
	EnableWindow(state.tabs, !running);
	EnableWindow(state.render_button, !running);
	EnableWindow(state.cancel_button, running);
	if (running)
	{
		set_marquee(state.progress, true);
		SendMessageW(state.progress, PBM_SETPOS, 0, 0);
		set_text(state.status_title, L"Preparing render…");
		set_text(state.status_detail, L"Validating files and settings");
		set_text(state.status_metrics, L"");
	}
}

void show_error(HWND owner, const std::wstring& message)
{
	MessageBoxW(owner, message.c_str(), L"SAFSYN Renderer", MB_OK | MB_ICONERROR);
}

bool parse_unsigned(HWND owner, HWND control, const wchar_t* name,
	uint64_t minimum, uint64_t maximum, uint64_t& output)
{
	const std::wstring value = trim(control_text(control));
	if (value.empty() || value.front() == L'-')
	{
		show_error(owner, std::wstring(name) + L" is required.");
		SetFocus(control);
		return false;
	}
	wchar_t* end = nullptr;
	errno = 0;
	const unsigned long long parsed = wcstoull(value.c_str(), &end, 10);
	if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
		parsed < minimum || parsed > maximum)
	{
		show_error(owner, std::wstring(name) + L" must be between " +
			std::to_wstring(minimum) + L" and " + std::to_wstring(maximum) + L".");
		SetFocus(control);
		return false;
	}
	output = static_cast<uint64_t>(parsed);
	return true;
}

bool parse_number(HWND owner, HWND control, const wchar_t* name,
	double minimum, double maximum, double& output, bool allow_empty = false)
{
	const std::wstring value = trim(control_text(control));
	if (value.empty() && allow_empty)
	{
		output = 0.0;
		return true;
	}
	wchar_t* end = nullptr;
	errno = 0;
	const double parsed = wcstod(value.c_str(), &end);
	if (value.empty() || errno == ERANGE || end == value.c_str() || *end != L'\0' ||
		!std::isfinite(parsed) || parsed < minimum || parsed > maximum)
	{
		std::wostringstream message;
		message << name << L" must be between " << minimum << L" and " << maximum << L'.';
		show_error(owner, message.str());
		SetFocus(control);
		return false;
	}
	output = parsed;
	return true;
}

bool read_request(AppState& state, RenderRequest& request)
{
	request.midi_path = trim(control_text(state.midi_path));
	request.bank_path = trim(control_text(state.bank_path));
	request.output_path = trim(control_text(state.output_path));
	if (request.midi_path.empty() || request.bank_path.empty() || request.output_path.empty())
	{
		show_error(state.window, L"Choose a MIDI file, a SoundFont/SFZ bank, and an output WAV.");
		return false;
	}
	if (lowercase_extension(request.midi_path) != L".mid")
	{
		show_error(state.window, L"The MIDI input must have a .mid extension.");
		return false;
	}
	const std::wstring bank_extension = lowercase_extension(request.bank_path);
	if (bank_extension != L".sf2" && bank_extension != L".sfz")
	{
		show_error(state.window, L"The sound bank must be an .sf2 or .sfz file.");
		return false;
	}
	if (lowercase_extension(request.output_path) != L".wav")
		request.output_path += L".wav";
	if (_wcsicmp(request.output_path.c_str(), request.midi_path.c_str()) == 0 ||
		_wcsicmp(request.output_path.c_str(), request.bank_path.c_str()) == 0)
	{
		show_error(state.window, L"The output path cannot overwrite an input file.");
		return false;
	}
	if (!std::filesystem::is_regular_file(std::filesystem::path(request.midi_path)) ||
		!std::filesystem::is_regular_file(std::filesystem::path(request.bank_path)))
	{
		show_error(state.window, L"One or more selected input files do not exist.");
		return false;
	}

	uint64_t sample_rate = 0, threads = 0, voices = 0, cohorts = 0;
	uint64_t bank = 0, program = 0, block = 0;
	if (!parse_unsigned(state.window, GetDlgItem(state.window, SampleRate),
		L"Sample rate", 8000, 384000, sample_rate) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, RenderThreads),
			L"Render threads", 1, 64, threads) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, VoiceCapacity),
			L"Voice capacity", 1, (std::numeric_limits<uint32_t>::max)(), voices) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, MaximumCohorts),
			L"Maximum cohorts", 0, (std::numeric_limits<uint32_t>::max)(), cohorts) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, InitialBank),
			L"Initial bank", 0, 16383, bank) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, InitialProgram),
			L"Initial program", 0, 127, program) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, BlockFrames),
			L"Block size", 1, 1'048'576, block))
		return false;

	double tail = 0.0, maximum = 0.0;
	const bool drain = SendDlgItemMessageW(state.window, TailMode, CB_GETCURSEL, 0, 0) == 1;
	if (!parse_number(state.window, GetDlgItem(state.window, TailSeconds),
		drain ? L"Maximum tail" : L"Tail length", drain ? 0.001 : 0.0,
		86400.0, tail) ||
		!parse_number(state.window, GetDlgItem(state.window, MaximumSeconds),
			L"Maximum render length", 0.001, 31536000.0, maximum, true))
		return false;

	auto& options = request.options;
	options.sample_rate = static_cast<uint32_t>(sample_rate);
	options.render_threads = static_cast<size_t>(threads);
	options.voice_capacity = static_cast<size_t>(voices);
	options.maximum_cohorts = static_cast<size_t>(cohorts);
	options.initial_bank = static_cast<uint16_t>(bank);
	options.initial_program = static_cast<uint8_t>(program);
	options.block_frames = static_cast<uint32_t>(block);
	options.voice_model = SendDlgItemMessageW(state.window, VoiceModel,
		CB_GETCURSEL, 0, 0) == 1 ? safsyn::VoiceModel::Individual : safsyn::VoiceModel::Cohorts;
	options.drain_tail = drain;
	if (drain)
	{
		if (!seconds_to_frames(tail, options.sample_rate, options.maximum_tail_frames))
			return false;
	}
	else if (!seconds_to_frames(tail, options.sample_rate, options.tail_frames))
		return false;
	if (maximum != 0.0 && !seconds_to_frames(maximum, options.sample_rate,
		options.maximum_frames))
		return false;
	options.all_regions = SendDlgItemMessageW(state.window, AllRegions,
		BM_GETCHECK, 0, 0) == BST_CHECKED;

	const int phase_mode = static_cast<int>(SendDlgItemMessageW(state.window,
		PhaseMode, CB_GETCURSEL, 0, 0));
	const safsyn::PhaseMode phase_modes[] = {safsyn::PhaseMode::Coherent,
		safsyn::PhaseMode::RandomPolarity, safsyn::PhaseMode::Analytic,
		safsyn::PhaseMode::SmoothField, safsyn::PhaseMode::IndependentBins};
	options.phase.mode = phase_modes[(std::max)(0, (std::min)(phase_mode, 4))];
	double phase_strength = 0.0, correlation = 0.0, attack = 0.0;
	uint64_t pool = 0, seed = 0;
	if (!parse_number(state.window, GetDlgItem(state.window, PhaseStrength),
		L"Phase strength", 0.0, 1.0, phase_strength) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, PhasePool),
			L"Phase pool", 1, 64, pool) ||
		!parse_unsigned(state.window, GetDlgItem(state.window, PhaseSeed),
			L"Phase seed", 0, (std::numeric_limits<uint64_t>::max)(), seed) ||
		!parse_number(state.window, GetDlgItem(state.window, PhaseCorrelation),
			L"Phase correlation", 0.001, 1000000.0, correlation) ||
		!parse_number(state.window, GetDlgItem(state.window, PhaseAttack),
			L"Preserved attack", 0.0, 60000.0, attack))
		return false;
	options.phase.strength = static_cast<float>(phase_strength);
	options.phase.pool_size = static_cast<uint32_t>(pool);
	options.phase.seed = seed;
	options.phase.correlation_hz = static_cast<float>(correlation);
	options.phase.preserve_attack_ms = static_cast<float>(attack);
	options.phase.continuous = SendDlgItemMessageW(state.window, PhaseContinuous,
		BM_GETCHECK, 0, 0) == BST_CHECKED;

	double gain = 0.0, ceiling = 0.0, lookahead = 0.0, release = 0.0;
	if (!parse_number(state.window, GetDlgItem(state.window, OutputGain),
		L"Output gain", -120.0, 120.0, gain) ||
		!parse_number(state.window, GetDlgItem(state.window, LimiterCeiling),
			L"Limiter ceiling", -120.0, 0.0, ceiling) ||
		!parse_number(state.window, GetDlgItem(state.window, LimiterLookahead),
			L"Limiter lookahead", 0.0, 1000.0, lookahead) ||
		!parse_number(state.window, GetDlgItem(state.window, LimiterRelease),
			L"Limiter release", 0.001, 60000.0, release))
		return false;
	options.mastering.output_gain_db = gain;
	options.mastering.limiter_enabled = SendDlgItemMessageW(state.window, LimiterEnabled,
		BM_GETCHECK, 0, 0) == BST_CHECKED;
	options.mastering.limiter_ceiling_db = ceiling;
	options.mastering.limiter_lookahead_ms = lookahead;
	options.mastering.limiter_release_ms = release;
	if (!safsyn::mastering_settings_valid(options.mastering))
	{
		show_error(state.window, L"The mastering settings are not valid.");
		return false;
	}
	set_text(state.output_path, request.output_path);
	return true;
}

bool post_stage(HWND window, std::wstring title, std::wstring detail)
{
	auto update = std::make_unique<StageUpdate>();
	update->title = std::move(title);
	update->detail = std::move(detail);
	if (!PostMessageW(window, kStageMessage, 0,
		reinterpret_cast<LPARAM>(update.get())))
		return false;
	update.release();
	return true;
}

bool progress_callback(const safsyn::SmfRenderProgress& progress,
	void* user_data) noexcept
{
	auto& context = *static_cast<CallbackContext*>(user_data);
	if (context.cancel->load(std::memory_order_relaxed)) return false;
	try
	{
		const auto now = std::chrono::steady_clock::now();
		const bool stage_changed = !context.posted || progress.stage != context.last_stage;
		if (stage_changed || progress.stage == safsyn::SmfRenderProgressStage::Complete ||
			now - context.last_posted >= std::chrono::milliseconds(50))
		{
			auto update = std::make_unique<ProgressUpdate>();
			update->progress = progress;
			update->elapsed_seconds = std::chrono::duration<double>(now - context.started).count();
			if (PostMessageW(context.window, kProgressMessage, 0,
				reinterpret_cast<LPARAM>(update.get())))
				update.release();
			context.last_posted = now;
			context.last_stage = progress.stage;
			context.posted = true;
		}
	}
	catch (...)
	{
	}
	return !context.cancel->load(std::memory_order_relaxed);
}

void post_done(HWND window, std::unique_ptr<DoneUpdate> update)
{
	if (PostMessageW(window, kDoneMessage, 0, reinterpret_cast<LPARAM>(update.get())))
		update.release();
}

void render_worker(HWND window, std::atomic_bool& cancel, RenderRequest request)
{
	const auto started = std::chrono::steady_clock::now();
	auto done = std::make_unique<DoneUpdate>();
	try
	{
		const std::string midi_path = std::filesystem::path(request.midi_path).string();
		const std::string bank_path = std::filesystem::path(request.bank_path).string();
		const std::string output_path = std::filesystem::path(request.output_path).string();
		post_stage(window, L"Reading MIDI…", L"Validating the file and track structure");
		safsyn::SmfFile midi;
		if (!midi.load(midi_path.c_str()))
		{
			done->error = diagnostics_text(midi.diagnostics());
			if (done->error.empty()) done->error = L"Could not read the selected MIDI file.";
			post_done(window, std::move(done));
			return;
		}
		if (cancel.load(std::memory_order_relaxed))
		{
			done->cancelled = true;
			post_done(window, std::move(done));
			return;
		}

		post_stage(window, L"Analyzing timeline…",
			L"Counting events and calculating the exact output duration");
		safsyn::SmfAnalysisOptions analysis_options;
		analysis_options.sample_rate = request.options.sample_rate;
		analysis_options.initial_bank = request.options.initial_bank;
		analysis_options.initial_program = request.options.initial_program;
		analysis_options.tail_frames = request.options.drain_tail
			? request.options.maximum_tail_frames : request.options.tail_frames;
		if (!safsyn::analyze_smf(midi, analysis_options, done->analysis))
		{
			done->error = diagnostics_text(done->analysis.diagnostics);
			if (done->error.empty()) done->error = L"The MIDI timeline could not be analyzed.";
			post_done(window, std::move(done));
			return;
		}
		if (cancel.load(std::memory_order_relaxed))
		{
			done->cancelled = true;
			post_done(window, std::move(done));
			return;
		}

		std::wostringstream bank_detail;
		bank_detail << done->analysis.header.track_count << L" tracks • "
			<< format_count(done->analysis.total_events) << L" events • "
			<< format_duration(static_cast<double>(done->analysis.duration_frames) /
				request.options.sample_rate) << L" source duration";
		post_stage(window, L"Loading sound bank…", bank_detail.str());
		safsyn::Soundfont soundfont;
		const std::wstring extension = lowercase_extension(request.bank_path);
		const bool loaded = extension == L".sfz"
			? safsyn::load_sfz(bank_path.c_str(), soundfont)
			: safsyn::load_sf2(bank_path.c_str(), soundfont);
		if (!loaded)
		{
			done->error = L"Could not load the selected sound bank.";
			post_done(window, std::move(done));
			return;
		}
		if (cancel.load(std::memory_order_relaxed))
		{
			done->cancelled = true;
			post_done(window, std::move(done));
			return;
		}

		CallbackContext callback_context;
		callback_context.window = window;
		callback_context.cancel = &cancel;
		callback_context.started = started;
		request.options.progress_callback = progress_callback;
		request.options.progress_user_data = &callback_context;
		done->success = safsyn::render_smf_stream(midi, done->analysis, soundfont,
			output_path.c_str(), request.options, done->result);
		done->cancelled = done->result.cancelled || cancel.load(std::memory_order_relaxed);
		if (!done->success && !done->cancelled)
		{
			done->error = diagnostics_text(done->result.diagnostics);
			if (done->error.empty()) done->error = L"Rendering failed for an unknown reason.";
		}
		std::error_code error;
		done->output_bytes = std::filesystem::file_size(
			std::filesystem::path(request.output_path), error);
		if (error) done->output_bytes = 0;
	}
	catch (const std::exception& error)
	{
		done->error = L"Renderer error: " + widen(error.what());
	}
	catch (...)
	{
		done->error = L"Renderer error: unexpected exception.";
	}
	done->elapsed_seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - started).count();
	post_done(window, std::move(done));
}

std::wstring choose_open_file(HWND owner, const wchar_t* filter, HWND source)
{
	std::vector<wchar_t> buffer(32768, L'\0');
	const std::wstring current = control_text(source);
	if (current.size() + 1 < buffer.size())
		std::copy(current.begin(), current.end(), buffer.begin());
	OPENFILENAMEW dialog{};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = owner;
	dialog.lpstrFilter = filter;
	dialog.lpstrFile = buffer.data();
	dialog.nMaxFile = static_cast<DWORD>(buffer.size());
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
		OFN_EXPLORER | OFN_HIDEREADONLY;
	return GetOpenFileNameW(&dialog) ? std::wstring(buffer.data()) : std::wstring{};
}

std::wstring choose_output_file(HWND owner, HWND source)
{
	std::vector<wchar_t> buffer(32768, L'\0');
	const std::wstring current = control_text(source);
	if (current.size() + 1 < buffer.size())
		std::copy(current.begin(), current.end(), buffer.begin());
	const wchar_t filter[] = L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
	OPENFILENAMEW dialog{};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = owner;
	dialog.lpstrFilter = filter;
	dialog.lpstrFile = buffer.data();
	dialog.nMaxFile = static_cast<DWORD>(buffer.size());
	dialog.lpstrDefExt = L"wav";
	dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
		OFN_EXPLORER;
	return GetSaveFileNameW(&dialog) ? std::wstring(buffer.data()) : std::wstring{};
}

void suggest_output(AppState& state)
{
	if (!trim(control_text(state.output_path)).empty()) return;
	const std::wstring midi = trim(control_text(state.midi_path));
	if (midi.empty()) return;
	std::filesystem::path output(midi);
	output.replace_extension(L".wav");
	set_text(state.output_path, output.wstring());
}

void handle_drop(AppState& state, HDROP drop)
{
	const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
	for (UINT index = 0; index < count; ++index)
	{
		const UINT length = DragQueryFileW(drop, index, nullptr, 0);
		std::wstring path(static_cast<size_t>(length) + 1, L'\0');
		DragQueryFileW(drop, index, path.data(), length + 1);
		path.resize(length);
		const std::wstring extension = lowercase_extension(path);
		if (extension == L".mid") set_text(state.midi_path, path);
		else if (extension == L".sf2" || extension == L".sfz")
			set_text(state.bank_path, path);
		else if (extension == L".wav") set_text(state.output_path, path);
	}
	DragFinish(drop);
	suggest_output(state);
}

void layout(AppState& state, int width, int height)
{
	const int path_width = (std::max)(300, width - 300);
	for (int id : {MidiPath, BankPath, OutputPath})
		SetWindowPos(GetDlgItem(state.window, id), nullptr, 150,
			id == MidiPath ? 86 : id == BankPath ? 126 : 166,
			path_width, 27, SWP_NOZORDER);
	for (int id : {BrowseMidi, BrowseBank, BrowseOutput})
		SetWindowPos(GetDlgItem(state.window, id), nullptr, width - 130,
			id == BrowseMidi ? 85 : id == BrowseBank ? 125 : 165,
			98, 29, SWP_NOZORDER);
	SetWindowPos(state.tabs, nullptr, 32, 210, width - 64, 310, SWP_NOZORDER);
	const int progress_y = height - 174;
	SetWindowPos(state.status_title, nullptr, 32, progress_y, width - 330, 28, SWP_NOZORDER);
	SetWindowPos(state.status_detail, nullptr, 32, progress_y + 29,
		width - 64, 22, SWP_NOZORDER);
	SetWindowPos(state.render_button, nullptr, width - 264, progress_y - 3,
		110, 36, SWP_NOZORDER);
	SetWindowPos(state.cancel_button, nullptr, width - 142, progress_y - 3,
		110, 36, SWP_NOZORDER);
	SetWindowPos(state.progress, nullptr, 32, progress_y + 57, width - 64, 22, SWP_NOZORDER);
	SetWindowPos(state.status_metrics, nullptr, 32, progress_y + 88,
		width - 64, 44, SWP_NOZORDER);
}

void create_ui(AppState& state)
{
	NONCLIENTMETRICSW metrics{sizeof(metrics)};
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
	metrics.lfMessageFont.lfHeight = -16;
	wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Segoe UI");
	state.font = CreateFontIndirectW(&metrics.lfMessageFont);
	LOGFONTW bold = metrics.lfMessageFont;
	bold.lfWeight = FW_SEMIBOLD;
	state.font_bold = CreateFontIndirectW(&bold);
	LOGFONTW title = bold;
	title.lfHeight = -27;
	state.font_title = CreateFontIndirectW(&title);
	state.background = CreateSolidBrush(RGB(246, 248, 251));

	create_control(state, L"STATIC", L"SAFSYN Renderer", SS_LEFT, 0, 0,
		32, 20, 500, 34, state.font_title);
	create_control(state, L"STATIC",
		L"Turn a Standard MIDI File and sound bank into deterministic stereo audio.",
		SS_LEFT, 0, 0, 32, 54, 800, 24);
	create_control(state, L"STATIC", L"MIDI input", SS_LEFT, 0, 0, 32, 89, 108, 22,
		state.font_bold);
	create_control(state, L"STATIC", L"Sound bank", SS_LEFT, 0, 0, 32, 129, 108, 22,
		state.font_bold);
	create_control(state, L"STATIC", L"Output WAV", SS_LEFT, 0, 0, 32, 169, 108, 22,
		state.font_bold);
	state.midi_path = create_control(state, L"EDIT", L"",
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, MidiPath, 150, 86, 600, 27);
	state.bank_path = create_control(state, L"EDIT", L"",
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, BankPath, 150, 126, 600, 27);
	state.output_path = create_control(state, L"EDIT", L"",
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, OutputPath, 150, 166, 600, 27);
	for (int id : {BrowseMidi, BrowseBank, BrowseOutput})
		create_control(state, L"BUTTON", L"Browse…", BS_PUSHBUTTON | WS_TABSTOP,
			0, id, 770, id == BrowseMidi ? 85 : id == BrowseBank ? 125 : 165, 98, 29);

	state.tabs = create_control(state, WC_TABCONTROLW, L"", WS_TABSTOP, 0,
		SettingsTabs, 32, 210, 896, 310);
	TCITEMW item{};
	item.mask = TCIF_TEXT;
	for (const wchar_t* name : {L"Essentials", L"Phase", L"Mastering"})
	{
		item.pszText = const_cast<wchar_t*>(name);
		TabCtrl_InsertItem(state.tabs, TabCtrl_GetItemCount(state.tabs), &item);
	}

	const int left_label = 54, left_field = 210, right_label = 500, right_field = 690;
	const int rows[] = {258, 296, 334, 372, 410, 448};
	add_page_label(state, 0, L"Sample rate", left_label, rows[0], 145);
	add_page_combo(state, 0, SampleRate, left_field, rows[0] - 3, 160,
		{L"44100", L"48000", L"96000", L"192000"}, 1);
	add_page_label(state, 0, L"Render threads", left_label, rows[1], 145);
	add_page_edit(state, 0, RenderThreads, L"1", left_field, rows[1] - 3, 160);
	add_page_label(state, 0, L"Voice model", left_label, rows[2], 145);
	add_page_combo(state, 0, VoiceModel, left_field, rows[2] - 3, 190,
		{L"Exact cohorts (recommended)", L"Individual voices"}, 0);
	add_page_label(state, 0, L"Initial MIDI bank", left_label, rows[3], 145);
	add_page_edit(state, 0, InitialBank, L"0", left_field, rows[3] - 3, 160);
	add_page_label(state, 0, L"Initial program", left_label, rows[4], 145);
	add_page_edit(state, 0, InitialProgram, L"0", left_field, rows[4] - 3, 160);
	add_page_label(state, 0, L"Block size (frames)", left_label, rows[5], 145);
	add_page_edit(state, 0, BlockFrames, L"256", left_field, rows[5] - 3, 160);

	add_page_label(state, 0, L"Individual voice limit", right_label, rows[0], 180);
	add_page_edit(state, 0, VoiceCapacity, L"256", right_field, rows[0] - 3, 160);
	add_page_label(state, 0, L"Cohort safety ceiling", right_label, rows[1], 180);
	add_page_edit(state, 0, MaximumCohorts, L"0", right_field, rows[1] - 3, 160);
	add_page_label(state, 0, L"Tail behavior", right_label, rows[2], 180);
	add_page_combo(state, 0, TailMode, right_field, rows[2] - 3, 190,
		{L"Fixed release tail", L"Drain active voices"}, 0);
	add_page_label(state, 0, L"Tail / maximum tail (s)", right_label, rows[3], 180);
	add_page_edit(state, 0, TailSeconds, L"2", right_field, rows[3] - 3, 160);
	add_page_label(state, 0, L"Maximum render (s)", right_label, rows[4], 180);
	add_page_edit(state, 0, MaximumSeconds, L"", right_field, rows[4] - 3, 160);
	add_page_check(state, 0, AllRegions, L"All-regions stress mode", right_label,
		rows[5] - 4, 250);

	add_page_label(state, 1,
		L"Coherent is the bit-exact baseline. Other modes are experimental phase-decorrelation policies.",
		54, rows[0], 820, 24);
	add_page_label(state, 1, L"Phase mode", left_label, rows[1], 145);
	add_page_combo(state, 1, PhaseMode, left_field, rows[1] - 3, 190,
		{L"Coherent", L"Random polarity", L"Analytic", L"Smooth field",
			L"Independent bins"}, 0);
	add_page_label(state, 1, L"Strength (0–1)", left_label, rows[2], 145);
	add_page_edit(state, 1, PhaseStrength, L"1", left_field, rows[2] - 3, 160);
	add_page_label(state, 1, L"Variant pool (1–64)", left_label, rows[3], 145);
	add_page_edit(state, 1, PhasePool, L"64", left_field, rows[3] - 3, 160);
	add_page_check(state, 1, PhaseContinuous, L"Continuous assignment", left_label,
		rows[4] - 4, 250);
	add_page_label(state, 1, L"Seed", right_label, rows[1], 180);
	add_page_edit(state, 1, PhaseSeed, L"0", right_field, rows[1] - 3, 160);
	add_page_label(state, 1, L"Correlation (Hz)", right_label, rows[2], 180);
	add_page_edit(state, 1, PhaseCorrelation, L"250", right_field, rows[2] - 3, 160);
	add_page_label(state, 1, L"Preserve attack (ms)", right_label, rows[3], 180);
	add_page_edit(state, 1, PhaseAttack, L"0", right_field, rows[3] - 3, 160);

	add_page_label(state, 2,
		L"Mastering follows the raw mix. Leave gain at 0 dB and limiter off for the reference output.",
		54, rows[0], 820, 24);
	add_page_label(state, 2, L"Output gain (dB)", left_label, rows[1], 145);
	add_page_edit(state, 2, OutputGain, L"0", left_field, rows[1] - 3, 160);
	add_page_check(state, 2, LimiterEnabled, L"Enable stereo-linked limiter", left_label,
		rows[2] - 4, 280);
	add_page_label(state, 2, L"Ceiling (dBFS)", right_label, rows[1], 180);
	add_page_edit(state, 2, LimiterCeiling, L"-0.3", right_field, rows[1] - 3, 160);
	add_page_label(state, 2, L"Lookahead (ms)", right_label, rows[2], 180);
	add_page_edit(state, 2, LimiterLookahead, L"5", right_field, rows[2] - 3, 160);
	add_page_label(state, 2, L"Release (ms)", right_label, rows[3], 180);
	add_page_edit(state, 2, LimiterRelease, L"100", right_field, rows[3] - 3, 160);
	show_page(state, 0);

	state.status_title = create_control(state, L"STATIC", L"Ready to render",
		SS_LEFT, 0, StatusTitle, 32, 546, 560, 28, state.font_bold);
	state.status_detail = create_control(state, L"STATIC",
		L"Choose the three files above, review settings, then start the render.",
		SS_LEFT, 0, StatusDetail, 32, 575, 800, 22);
	state.render_button = create_control(state, L"BUTTON", L"Render WAV",
		BS_DEFPUSHBUTTON | WS_TABSTOP, 0, RenderButton, 696, 543, 110, 36,
		state.font_bold);
	state.cancel_button = create_control(state, L"BUTTON", L"Cancel",
		BS_PUSHBUTTON | WS_TABSTOP, 0, CancelButton, 818, 543, 110, 36);
	EnableWindow(state.cancel_button, FALSE);
	state.progress = create_control(state, PROGRESS_CLASSW, L"",
		PBS_SMOOTH, 0, ProgressBar, 32, 603, 896, 22);
	SendMessageW(state.progress, PBM_SETRANGE32, 0, 1000);
	state.status_metrics = create_control(state, L"STATIC",
		L"Progress will show frames, events, active voices, peak level, and timing.",
		SS_LEFT, 0, StatusMetrics, 32, 634, 896, 44);

	state.input_controls.insert(state.input_controls.end(), {
		state.midi_path, state.bank_path, state.output_path,
		GetDlgItem(state.window, BrowseMidi), GetDlgItem(state.window, BrowseBank),
		GetDlgItem(state.window, BrowseOutput)});
	DragAcceptFiles(state.window, TRUE);
}

void update_progress_ui(AppState& state, const ProgressUpdate& update)
{
	set_marquee(state.progress, false);
	const auto& progress = update.progress;
	const wchar_t* stage = L"Preparing render…";
	switch (progress.stage)
	{
	case safsyn::SmfRenderProgressStage::Preparing: stage = L"Preparing audio engine…"; break;
	case safsyn::SmfRenderProgressStage::RenderingEvents: stage = L"Rendering MIDI events…"; break;
	case safsyn::SmfRenderProgressStage::RenderingTail: stage = L"Rendering release tail…"; break;
	case safsyn::SmfRenderProgressStage::Finalizing: stage = L"Finalizing WAV…"; break;
	case safsyn::SmfRenderProgressStage::Complete: stage = L"Render complete"; break;
	}
	set_text(state.status_title, stage);
	const double fraction = progress.total_frames == 0 ? 0.0 :
		(static_cast<double>(progress.frames_rendered) / progress.total_frames);
	const int position = static_cast<int>((std::max)(0.0, (std::min)(1.0, fraction)) * 1000.0);
	SendMessageW(state.progress, PBM_SETPOS, position, 0);

	std::wostringstream detail;
	detail << static_cast<int>(fraction * 100.0) << L"%  •  "
		<< format_count(progress.frames_rendered) << L" / "
		<< format_count(progress.total_frames) << L" frames  •  elapsed "
		<< format_duration(update.elapsed_seconds);
	if (fraction > 0.002 && fraction < 1.0)
		detail << L"  •  about " << format_duration(update.elapsed_seconds / fraction -
			update.elapsed_seconds) << L" remaining";
	set_text(state.status_detail, detail.str());

	std::wostringstream live;
	live << format_count(progress.scheduled_events) << L" scheduled events  •  "
		<< format_count(progress.active_voices) << L" active voices  •  "
		<< format_count(progress.active_cohorts) << L" active cohorts  •  peak "
		<< format_peak(progress.raw_peak);
	set_text(state.status_metrics, live.str());
}

void handle_done(AppState& state, DoneUpdate& done)
{
	if (state.worker.joinable()) state.worker.join();
	set_running(state, false);
	set_marquee(state.progress, false);
	if (done.cancelled)
	{
		set_text(state.status_title, L"Render cancelled");
		std::wstring detail = done.result.frames_written == 0
			? L"No audio was rendered."
			: L"A valid partial WAV was finalized with " +
				format_count(done.result.frames_written) + L" frames.";
		set_text(state.status_detail, detail);
		set_text(state.status_metrics, L"Adjust the settings or choose Render WAV to try again.");
	}
	else if (!done.success)
	{
		set_text(state.status_title, L"Render failed");
		set_text(state.status_detail, done.error.empty() ? L"The render could not be completed." : done.error);
		set_text(state.status_metrics, L"The output may be incomplete. Review the input files and settings.");
		SendMessageW(state.progress, PBM_SETSTATE, PBST_ERROR, 0);
	}
	else
	{
		SendMessageW(state.progress, PBM_SETPOS, 1000, 0);
		SendMessageW(state.progress, PBM_SETSTATE, PBST_NORMAL, 0);
		set_text(state.status_title, L"Render complete");
		std::wostringstream detail;
		detail << format_duration(static_cast<double>(done.result.frames_written) /
			done.analysis.sample_rate) << L" audio  •  " << format_bytes(done.output_bytes)
			<< L"  •  completed in " << format_duration(done.elapsed_seconds);
		set_text(state.status_detail, detail.str());
		std::wostringstream summary;
		summary << format_count(done.result.scheduled_events) << L" events  •  "
			<< format_count(done.result.engine.peak_active_logical_voices)
			<< L" peak logical voices  •  peak " << format_peak(done.result.peak)
			<< L"  •  audio engine " << format_duration(done.result.render_ms / 1000.0);
		set_text(state.status_metrics, summary.str());
	}
	if (state.closing) DestroyWindow(state.window);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	AppState* state = state_from(window);
	switch (message)
	{
	case WM_NCCREATE:
	{
		auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
		state = static_cast<AppState*>(created->lpCreateParams);
		state->window = window;
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
		return TRUE;
	}
	case WM_CREATE:
		create_ui(*state);
		return 0;
	case WM_GETMINMAXINFO:
	{
		auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
		limits->ptMinTrackSize = {940, 720};
		return 0;
	}
	case WM_SIZE:
		if (state) layout(*state, LOWORD(lparam), HIWORD(lparam));
		return 0;
	case WM_CTLCOLORSTATIC:
		if (state)
		{
			SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
			SetTextColor(reinterpret_cast<HDC>(wparam), RGB(31, 41, 55));
			return reinterpret_cast<LRESULT>(state->background);
		}
		break;
	case WM_ERASEBKGND:
		if (state && state->background)
		{
			RECT bounds{};
			GetClientRect(window, &bounds);
			FillRect(reinterpret_cast<HDC>(wparam), &bounds, state->background);
			return 1;
		}
		break;
	case WM_NOTIFY:
		if (state && reinterpret_cast<NMHDR*>(lparam)->idFrom == SettingsTabs &&
			reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE)
		{
			show_page(*state, TabCtrl_GetCurSel(state->tabs));
			return 0;
		}
		break;
	case WM_DROPFILES:
		if (state && !state->running) handle_drop(*state, reinterpret_cast<HDROP>(wparam));
		return 0;
	case WM_COMMAND:
		if (!state) break;
		switch (LOWORD(wparam))
		{
		case BrowseMidi:
		{
			const wchar_t filter[] = L"MIDI files (*.mid)\0*.mid\0All files (*.*)\0*.*\0\0";
			const std::wstring path = choose_open_file(window, filter, state->midi_path);
			if (!path.empty()) { set_text(state->midi_path, path); suggest_output(*state); }
			return 0;
		}
		case BrowseBank:
		{
			const wchar_t filter[] =
				L"Sound banks (*.sf2;*.sfz)\0*.sf2;*.sfz\0SoundFont 2 (*.sf2)\0*.sf2\0SFZ (*.sfz)\0*.sfz\0All files (*.*)\0*.*\0\0";
			const std::wstring path = choose_open_file(window, filter, state->bank_path);
			if (!path.empty()) set_text(state->bank_path, path);
			return 0;
		}
		case BrowseOutput:
		{
			const std::wstring path = choose_output_file(window, state->output_path);
			if (!path.empty()) set_text(state->output_path, path);
			return 0;
		}
		case RenderButton:
		{
			RenderRequest request;
			if (!read_request(*state, request)) return 0;
			if (state->worker.joinable()) state->worker.join();
			state->cancel.store(false, std::memory_order_relaxed);
			SendMessageW(state->progress, PBM_SETSTATE, PBST_NORMAL, 0);
			set_running(*state, true);
			state->worker = std::thread(render_worker, window, std::ref(state->cancel),
				std::move(request));
			return 0;
		}
		case CancelButton:
			state->cancel.store(true, std::memory_order_relaxed);
			EnableWindow(state->cancel_button, FALSE);
			set_text(state->status_title, L"Cancelling…");
			set_text(state->status_detail, L"Finishing the current block and finalizing partial audio.");
			return 0;
		}
		break;
	case kStageMessage:
		if (state)
		{
			std::unique_ptr<StageUpdate> update(reinterpret_cast<StageUpdate*>(lparam));
			set_marquee(state->progress, true);
			set_text(state->status_title, update->title);
			set_text(state->status_detail, update->detail);
		}
		return 0;
	case kProgressMessage:
		if (state)
		{
			std::unique_ptr<ProgressUpdate> update(reinterpret_cast<ProgressUpdate*>(lparam));
			update_progress_ui(*state, *update);
		}
		return 0;
	case kDoneMessage:
		if (state)
		{
			std::unique_ptr<DoneUpdate> update(reinterpret_cast<DoneUpdate*>(lparam));
			handle_done(*state, *update);
		}
		return 0;
	case WM_CLOSE:
		if (state && state->running)
		{
			if (MessageBoxW(window, L"Cancel the active render and close the window?",
				L"SAFSYN Renderer", MB_YESNO | MB_ICONQUESTION) == IDYES)
			{
				state->closing = true;
				state->cancel.store(true, std::memory_order_relaxed);
				EnableWindow(state->cancel_button, FALSE);
				set_text(state->status_title, L"Cancelling before close…");
			}
			return 0;
		}
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		if (state)
		{
			if (state->worker.joinable()) state->worker.join();
			DeleteObject(state->font);
			DeleteObject(state->font_bold);
			DeleteObject(state->font_title);
			DeleteObject(state->background);
		}
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
	INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES |
		ICC_PROGRESS_CLASS | ICC_TAB_CLASSES};
	InitCommonControlsEx(&controls);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	WNDCLASSEXW window_class{sizeof(window_class)};
	window_class.style = CS_HREDRAW | CS_VREDRAW;
	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = instance;
	window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	window_class.hIconSm = window_class.hIcon;
	window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	window_class.lpszClassName = kWindowClass;
	if (!RegisterClassExW(&window_class)) return 1;

	AppState state;
	HWND window = CreateWindowExW(0, kWindowClass, L"SAFSYN Renderer",
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
		980, 760, nullptr, nullptr, instance, &state);
	if (!window) return 1;
	ShowWindow(window, show_command);
	UpdateWindow(window);

	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessageW(window, &message))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	return static_cast<int>(message.wParam);
}
