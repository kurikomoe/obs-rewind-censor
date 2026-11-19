#include <obs-module.h>
#include "../src/indicator.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <util/platform.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr int width = 96, height = 64;
enum class Picture { other, black, stripes, mosaic, fit, blue };
struct Sample {
	uint64_t ts;
	Picture picture;
	std::array<uint32_t, 3> rgb;
};
std::vector<Sample> output;
std::mutex samples_lock;
std::atomic<int> audio_blocks{0}, audio_errors{0};
std::atomic<int> audible_window_blocks{0};
int failures = 0;
int audio_delay_blocks = 100;

void check(bool ok, const char *label)
{
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
	if (!ok)
		++failures;
}

void read_video(void *, video_data *frame)
{
	auto pixel = [&](int x, int y) {
		auto *p = frame->data[0] + frame->linesize[0] * y + 4 * x;
		if (p[2] > 150 && p[1] < 60)
			return 'r';
		if (p[1] > 150 && p[2] < 60)
			return 'g';
		if (p[0] > 150 && p[2] < 60 && p[1] < 60)
			return 'b';
		if (std::max({p[0], p[1], p[2]}) < 15)
			return 'k';
		return '?';
	};
	const char left = pixel(5, 30), center = pixel(48, 30), right = pixel(90, 30);
	auto rgb = [&](int x) {
		auto *p = frame->data[0] + 30 * frame->linesize[0] + 4 * x;
		return uint32_t(p[2] << 16 | p[1] << 8 | p[0]);
	};
	Picture pic = Picture::other;
	if (left == 'k' && right == 'k')
		pic = center == 'b' && pixel(48, 2) == 'b' && pixel(48, 61) == 'b' && pixel(34, 30) == 'b' &&
			      pixel(29, 30) == 'k' && pixel(67, 30) == 'k'
			  ? Picture::fit
			  : (center == 'k' ? Picture::black : Picture::other);
	if (left == 'r' && right == 'g')
		pic = Picture::stripes;
	if (left == 'b' && right == 'b' && center == 'b')
		pic = Picture::blue;
	if (rgb(5) == rgb(48) && rgb(48) == rgb(90) && (rgb(48) & 0xffff00) > 0x3000)
		pic = Picture::mosaic;
	std::lock_guard lock(samples_lock);
	output.push_back({frame->timestamp, pic, {rgb(5), rgb(48), rgb(90)}});
}

void expect(uint64_t base, double from, double to, Picture kind, const char *label)
{
	std::lock_guard lock(samples_lock);
	int total = 0, matching = 0;
	for (const auto &s : output) {
		const double t = (double(s.ts) - double(base)) / 1e9;
		if (t >= from && t <= to) {
			if (!total && s.picture != kind)
				std::printf("  left/center/right RGB: %06x %06x %06x\n", s.rgb[0], s.rgb[1],
					    s.rgb[2]);
			++total;
			matching += s.picture == kind;
		}
	}
	if (total < 2 || total != matching)
		std::printf("  matched %d of %d frames\n", matching, total);
	check(total >= 2 && total == matching, label);
}

// A 1 Hz square wave makes every outgoing block testable independently.
void read_audio(void *, obs_source_t *, const audio_data *a, bool)
{
	const int index = audio_blocks++;
	const float expected =
	    index < audio_delay_blocks ? 0.f : ((index - audio_delay_blocks) % 100 < 50 ? 0.4f : -0.4f);
	for (size_t ch = 0; ch < 2; ++ch) {
		const auto *samples = reinterpret_cast<const float *>(a->data[ch]);
		for (uint32_t i = 0; i < a->frames; ++i) {
			if (std::abs(samples[i] - expected) > 0.001f) {
				++audio_errors;
				break;
			}
		}
	}
}

void tap(obs_key_t button)
{
	obs_key_combination_t key{};
	key.key = button;
	obs_hotkey_inject_event(key, true);
	obs_hotkey_inject_event(key, false);
}

void check_status(obs_source_t *filter, bool marking, PictureState picture, int64_t min_ns, int64_t max_ns,
		  const char *message)
{
	calldata_t state{};
	const bool read = proc_handler_call(obs_source_get_proc_handler(filter), "censor_status", &state);
	const auto buffered = calldata_int(&state, "buffered_ns");
	check(read && calldata_bool(&state, "marking") == marking &&
	      calldata_int(&state, "picture") == static_cast<int>(picture) &&
	      buffered >= min_ns && buffered <= max_ns &&
	      calldata_int(&state, "delay_ns") == 1000000000 && calldata_int(&state, "history_ns") == 500000000,
	      message);
	calldata_free(&state);
}

obs_hotkey_id filter_key(obs_source_t *filter, const char *field)
{
	calldata_t state{};
	long long id = -1;
	if (proc_handler_call(obs_source_get_proc_handler(filter), "censor_status", &state))
		calldata_get_int(&state, field, &id);
	calldata_free(&state);
	return static_cast<obs_hotkey_id>(id);
}

bool bound_to(obs_hotkey_id id, const char *expected)
{
	auto *keys = obs_hotkey_save(id);
	auto *first = obs_data_array_item(keys, 0);
	const bool correct = expected ? obs_data_array_count(keys) == 1 && first &&
					    std::strcmp(obs_data_get_string(first, "key"), expected) == 0
				      : obs_data_array_count(keys) == 0;
	obs_data_release(first);
	obs_data_array_release(keys);
	return id != OBS_INVALID_HOTKEY_ID && correct;
}

void check_direct_actions(obs_source_t *filter)
{
	const auto set = filter_key(filter, "set_key"), cancel = filter_key(filter, "cancel_key");
	check(bound_to(set, nullptr) && bound_to(cancel, nullptr), "both direct actions start unbound");
	obs_key_combination_t key{};
	key.key = OBS_KEY_F7;
	obs_hotkey_load_bindings(set, &key, 1);
	key.key = OBS_KEY_F8;
	obs_hotkey_load_bindings(cancel, &key, 1);
	for (auto button : {OBS_KEY_F8, OBS_KEY_F8, OBS_KEY_F7, OBS_KEY_F7, OBS_KEY_F8}) {
		// libobs releases injected bindings on its next keyboard polling tick.
		std::this_thread::sleep_for(100ms);
		tap(button);
		calldata_t state{};
		const bool read = proc_handler_call(obs_source_get_proc_handler(filter), "censor_status", &state);
		check(read && calldata_bool(&state, "marking") == (button == OBS_KEY_F7),
		      button == OBS_KEY_F7 ? "enable always sets marking" : "disable always clears marking");
		calldata_free(&state);
	}
}

void check_saved_binding(obs_source_t *parent, obs_source_t *&filter, bool clear)
{
	obs_key_combination_t key{};
	key.key = OBS_KEY_F6;
	obs_hotkey_load_bindings(filter_key(filter, "key"), &key, clear ? 0 : 1);
	auto *saved = obs_save_source(filter);
	obs_source_filter_remove(parent, filter);
	obs_source_release(filter);
	obs_wait_for_destroy_queue();
	filter = obs_load_private_source(saved);
	obs_data_release(saved);
	obs_source_filter_add(parent, filter);
	check(bound_to(filter_key(filter, "key"), clear ? nullptr : "OBS_KEY_F6"),
	      clear ? "unbound hotkey stays empty after reload" : "custom hotkey survives filter reload");
	check(bound_to(filter_key(filter, "set_key"), "OBS_KEY_F7") &&
	      bound_to(filter_key(filter, "cancel_key"), "OBS_KEY_F8"), "direct hotkeys survive filter reload");
}

void write_portrait(const fs::path &path)
{
	// 2 x 4 24-bit blue BMP with rows padded to eight bytes.
	std::array<uint8_t, 86> bmp{};
	bmp[0] = 'B';
	bmp[1] = 'M';
	bmp[2] = 86;
	bmp[10] = 54;
	bmp[14] = 40;
	bmp[18] = 2;
	bmp[22] = 4;
	bmp[26] = 1;
	bmp[28] = 24;
	for (int y = 0; y < 4; ++y)
		bmp[54 + y * 8] = bmp[57 + y * 8] = 255;
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bmp.data()), bmp.size());
}

int main(int argc, char **argv)
{
	if (argc != 4 && argc != 5) {
		std::puts(
		    "usage: capture-test <OBS root> <plugin DLL> <plugin data directory> [--window|--1080p60]");
		return 2;
	}
	const bool use_window = argc == 5 && std::string(argv[4]) == "--window";
	const bool full_hd = argc == 5 && std::string(argv[4]) == "--1080p60";
	const uint32_t source_width = full_hd ? 1920 : width, source_height = full_hd ? 1080 : height;
	if (full_hd)
		audio_delay_blocks = 1000;
	const fs::path root = fs::absolute(argv[1]), dll = fs::absolute(argv[2]), data = fs::absolute(argv[3]);
	const auto test_dir = fs::current_path() / "build" / "capture-test-data";
	fs::create_directories(test_dir);
	const auto portrait = test_dir / "portrait.bmp";
	write_portrait(portrait);
	fs::current_path(root / "bin/64bit");
#ifdef _WIN32
	AddDllDirectory(fs::current_path().c_str());
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
	if (!obs_startup("en-US", test_dir.string().c_str(), nullptr))
		return 2;
	std::printf("Testing libobs %s\n", obs_get_version_string());
	obs_add_data_path((root / "data/libobs").string().c_str());
	obs_audio_info ai{48000, SPEAKERS_STEREO};
	obs_video_info vi{};
	const std::string d3d = (root / "bin/64bit/libobs-d3d11.dll").string();
	vi.graphics_module = d3d.c_str();
	vi.base_width = vi.output_width = source_width;
	vi.base_height = vi.output_height = source_height;
	vi.fps_num = 60;
	vi.fps_den = 1;
	vi.output_format = VIDEO_FORMAT_NV12;
	vi.colorspace = VIDEO_CS_709;
	vi.range = VIDEO_RANGE_PARTIAL;
	vi.gpu_conversion = true;
	if (!obs_reset_audio(&ai) || obs_reset_video(&vi) != OBS_VIDEO_SUCCESS)
		return 2;
	obs_module_t *module = nullptr;
	if (obs_open_module(&module, dll.string().c_str(), data.string().c_str()) != MODULE_SUCCESS ||
	    !obs_init_module(module))
		return 2;
	obs_source_info si{};
	si.id = "5050_test_capture";
	si.type = OBS_SOURCE_TYPE_INPUT;
	si.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
	si.get_name = [](void *) { return "Test capture"; };
	si.create = [](obs_data_t *, obs_source_t *s) -> void * { return s; };
	si.destroy = [](void *) {};
	obs_register_source(&si);
	obs_source_t *parent;
	if (use_window) {
		// Load the real OBS 33 Windows capture and per-application WASAPI modules.
		for (const std::string name : {"win-wasapi", "win-capture"}) {
			const auto path = root / "core" / name;
			obs_module_t *capture_module = nullptr;
			if (obs_open_module(&capture_module, (path / (name + ".dll")).string().c_str(),
					    (path / "data").string().c_str()) != MODULE_SUCCESS ||
			    !obs_init_module(capture_module))
				return 2;
		}
		auto *wc = obs_data_create();
		obs_data_set_string(wc, "window", "5050 local test:CensorCaptureFixture:capture-fixture.exe");
		obs_data_set_int(wc, "method", 2); // Windows Graphics Capture.
		obs_data_set_bool(wc, "cursor", false);
		obs_data_set_bool(wc, "capture_audio", true);
		parent = obs_source_create("window_capture", "Test window with application audio", wc, nullptr);
		obs_data_release(wc);
	} else {
		parent = obs_source_create(si.id, "Synthetic capture card", nullptr, nullptr);
	}
	if (!parent)
		return 2;
	video_scale_info format{VIDEO_FORMAT_BGRA, width, height, VIDEO_RANGE_FULL, VIDEO_CS_709};
	obs_add_raw_video_callback(&format, read_video, nullptr);
	if (use_window) {
		obs_set_output_source(0, parent);
		for (int i = 0; i < 70; ++i) {
			std::this_thread::sleep_for(100ms);
			std::lock_guard lock(samples_lock);
			if (!output.empty() && output.back().picture == Picture::stripes)
				break;
		}
		std::lock_guard lock(samples_lock);
		check(!output.empty() && output.back().picture == Picture::stripes,
		      "original window outputs the reference pattern");
	}
	auto *settings = obs_data_create();
	if (!full_hd) {
		obs_data_set_double(settings, "delay", 1.0);
		obs_data_set_double(settings, "history", 0.5);
	}
	auto *defaults = obs_get_source_defaults("5050_censor_filter");
	check(obs_data_get_int(defaults, "strength") == 40, "default mosaic strength is 40");
	obs_data_release(defaults);
	obs_data_set_int(settings, "strength", 100);
	auto *filter = obs_source_create_private("5050_censor_filter", "Censor", settings);
	obs_source_filter_add(parent, filter);
	check(obs_source_filter_count(parent) == 1, "single filter attached to the A/V source");
	auto *saved = obs_save_source(filter);
	auto *saved_settings = obs_data_get_obj(saved, "settings");
	check(std::strstr(obs_data_get_json(saved_settings), "OBS_KEY_F2") != nullptr,
	      "default F2 is registered and saved");
	obs_data_release(saved_settings);
	obs_data_release(saved);
	obs_set_output_source(0, parent);
	auto read_window_audio = +[](void *, obs_source_t *, const audio_data *a, bool) {
		++audio_blocks;
		auto *samples = reinterpret_cast<const float *>(a->data[0]);
		for (uint32_t i = 0; i < a->frames; ++i)
			if (std::abs(samples[i]) > 0.00002f) {
				++audible_window_blocks;
				break;
			}
	};
	auto audio_cb = use_window ? read_window_audio : read_audio;
	obs_source_add_audio_capture_callback(parent, audio_cb, nullptr);
	if (use_window) {
		for (int i = 0; i < 50 && obs_source_get_base_width(parent) != width; ++i)
			std::this_thread::sleep_for(100ms);
		check(obs_source_get_base_width(parent) == width, "actual window of the fixture was acquired");
	}
	const uint64_t start = os_gettime_ns();
	std::atomic_bool running{true};
	std::thread feed([&] {
		if (use_window)
			return;
		std::vector<uint8_t> pixels(source_width * source_height * 4), flash(pixels.size());
		for (uint32_t i = 0; i < source_width * source_height; ++i) {
			pixels[i * 4 + ((i % source_width < source_width / 2) ? 2 : 1)] = 255;
			pixels[i * 4 + 3] = 255;
			flash[i * 4] = flash[i * 4 + 3] = 255;
		}
		obs_source_frame v{};
		v.format = VIDEO_FORMAT_BGRA;
		v.full_range = true;
		v.width = source_width;
		v.height = source_height;
		v.linesize[0] = source_width * 4;
		std::array<float, 480> plane{};
		obs_source_audio a{};
		a.frames = 480;
		a.samples_per_sec = 48000;
		a.speakers = SPEAKERS_STEREO;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.data[0] = a.data[1] = reinterpret_cast<uint8_t *>(plane.data());
		for (int i = 0; running; ++i) {
			const uint64_t ts = start + uint64_t(i) * 10000000;
			os_sleepto_ns(ts);
			plane.fill(i % 100 < 50 ? 0.4f : -0.4f);
			a.timestamp = v.timestamp = ts;
			v.data[0] = full_hd && i >= 20 && i <= 70 ? flash.data() : pixels.data();
			obs_source_output_audio(parent, &a);
			if (i % 2 == 0)
				obs_source_output_video(parent, &v);
		}
	});
	if (full_hd) {
		std::this_thread::sleep_for(11s);
		auto on = os_gettime_ns();
		tap(OBS_KEY_F2);
		std::this_thread::sleep_for(800ms);
		auto off = os_gettime_ns();
		tap(OBS_KEY_F2);
		std::this_thread::sleep_for(10500ms);
		expect(start, 8.5, 9.8, Picture::black, "1080p60 warm-up lasts the default ten seconds");
		expect(start, 10.35, 10.60, Picture::blue, "brief input flash plays ten seconds later");
		expect(on, 3.5, 4.8, Picture::stripes,
		       "frames preceding the five-second lookback remain clear");
		expect(on, 5.3, 9.8, Picture::mosaic, "default lookback covers five seconds at 1080p60");
		expect(off, 10.1, 10.3, Picture::stripes, "1080p60 playout resumes on time");
	} else {
		std::this_thread::sleep_for(550ms);
		if (!use_window)
			check_status(filter, false, PictureState::buffering, 1, 900000000,
				"indicator reads the partially filled video queue");
		std::this_thread::sleep_for(1150ms);
		auto onset = os_gettime_ns();
		tap(OBS_KEY_F2);
		check_status(filter, true, PictureState::clear, 950000000, 1100000000,
			"indicator shows marking immediately while the delayed picture is still clear");
		std::this_thread::sleep_for(350ms);
		auto stop = os_gettime_ns();
		tap(OBS_KEY_F2);
		std::this_thread::sleep_for(500ms);
		check_status(filter, false, PictureState::mosaic, 950000000, 1100000000,
			"indicator shows the queued mosaic after marking has ended");
		std::this_thread::sleep_for(950ms);
		expect(start, .2, .8, Picture::black, "warm-up is black");
		expect(onset, -.2, .35, Picture::stripes, "older uncensored frames play normally");
		expect(onset, .60, .90, Picture::mosaic,
		       "F2 retroactively covers frames captured before the keypress");
		expect(stop, .2, .85, Picture::mosaic, "second F2 preserves marked frames in flight");
		expect(stop, 1.10, 1.30, Picture::stripes, "clear frames resume after the marked interval");
		obs_data_set_string(settings, "image", portrait.string().c_str());
		obs_source_update(filter, settings);
		tap(OBS_KEY_F2);
		auto image_start = os_gettime_ns();
		std::this_thread::sleep_for(1200ms);
		check_status(filter, true, PictureState::image, 950000000, 1100000000, "indicator reports image replacement");
		expect(image_start, .7, .95, Picture::fit,
		       "portrait image has the correct scale and opaque black pillarboxes");
		auto *props = obs_source_properties(filter);
		auto *clear = obs_properties_get(props, "clear_image");
		check(clear && obs_property_button_clicked(clear, filter), "clear image button requests a picker refresh");
		obs_properties_destroy(props);
		auto cleared = os_gettime_ns();
		std::this_thread::sleep_for(650ms);
		expect(cleared, .2, .45, Picture::mosaic, "clearing the image restores mosaic during active censorship");
		auto *serialized = obs_save_source(filter);
		auto *stored = obs_data_get_obj(serialized, "settings");
		check(!*obs_data_get_string(stored, "image") && obs_data_get_int(stored, "strength") == 100,
		      "cleared path and custom strength are persisted");
		obs_data_release(stored);
		obs_data_release(serialized);
		auto *change = obs_data_create();
		obs_data_set_int(change, "strength", 0);
		obs_source_update(filter, change);
		obs_data_release(change);
		auto zero = os_gettime_ns();
		std::this_thread::sleep_for(650ms);
		expect(zero, .2, .45, Picture::stripes, "zero mosaic strength preserves the frame");
		if (!use_window)
			check_direct_actions(filter);
	}
	running = false;
	feed.join();
	if (use_window)
		check(audio_blocks > 0 && audible_window_blocks > 0,
		      "real per-application audio reaches the window filter output");
	else
		check(audio_blocks > 400 && audio_errors == 0,
		      "both audio planes have the exact configured sample delay");
	obs_remove_raw_video_callback(read_video, nullptr);
	obs_source_remove_audio_capture_callback(parent, audio_cb, nullptr);
	obs_set_output_source(0, nullptr);
	if (!use_window && !full_hd) {
		obs_source_set_enabled(filter, false);
		check_status(filter, false, PictureState::bypass, 0, 0, "disabled filter reports bypass and clears stale buffer status");
		check_saved_binding(parent, filter, false);
		check_saved_binding(parent, filter, true);
	}
	obs_source_filter_remove(parent, filter);
	obs_source_release(filter);
	obs_source_release(parent);
	obs_data_release(settings);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return failures ? 1 : 0;
}
