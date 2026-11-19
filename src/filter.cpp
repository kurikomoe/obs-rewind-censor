#include "video.hpp"
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("5050-censor-filter", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Delayed capture with retroactive full-frame mosaic or image replacement";
}

namespace
{
constexpr uint64_t second = 1000000000ULL;
constexpr uint64_t open_end = std::numeric_limits<uint64_t>::max();
constexpr int max_seconds = 30;
constexpr int default_strength = 40;
struct Interval {
	uint64_t start, end;
};

enum Command { toggle_mark, set_mark, cancel_mark, command_count };
struct KeySpec { const char *prefix, *label, *setting; };
constexpr KeySpec commands[command_count] = {
	{"5050-censor.toggle.", "Toggle", "toggle_binding"},
	{"5050-censor.set.", "SetMark", "set_binding"},
	{"5050-censor.cancel.", "CancelMark", "cancel_binding"},
};

struct AudioBuffer {
	std::vector<float> samples;
	size_t length = 0, cursor = 0, channels = 0;
	uint64_t next_timestamp = 0, generation = 0;
	uint32_t rate = 0;

	void process(obs_audio_data *audio, uint64_t delay, uint64_t version)
	{
		auto *output = obs_get_audio();
		if (!output || !audio->frames)
			return;
		const auto hz = audio_output_get_sample_rate(output);
		const auto count = audio_output_get_channels(output);
		const auto frames = size_t((delay * hz + second / 2) / second);
		if (generation != version || rate != hz || channels != count || length != frames) {
			length = frames;
			channels = count;
			rate = hz;
			generation = version;
			samples.assign(length * channels, 0);
			cursor = 0;
			next_timestamp = 0;
		}
		if (!length || !channels)
			return;
		// Device timestamps may use a different epoch from the video clock.
		// Use them only for gaps. Preserve the current block's original PTS.
		const uint64_t ts = audio->timestamp;
		const uint64_t diff = ts > next_timestamp ? ts - next_timestamp : next_timestamp - ts;
		if (next_timestamp && diff > second / 50) {
			if (ts < next_timestamp || diff >= delay) {
				std::fill(samples.begin(), samples.end(), 0.0f);
				cursor = 0;
			} else {
				size_t missing = std::min(length, size_t((diff * hz + second / 2) / second));
				while (missing) {
					const size_t n = std::min(missing, length - cursor);
					for (size_t ch = 0; ch < channels; ++ch)
						std::fill_n(samples.data() + ch * length + cursor, n, 0.0f);
					cursor = (cursor + n) % length;
					missing -= n;
				}
			}
		}
		next_timestamp = ts + uint64_t(audio->frames) * second / hz;
		for (size_t offset = 0; offset < audio->frames;) {
			const size_t n = std::min(length - cursor, size_t(audio->frames) - offset);
			for (size_t ch = 0; ch < channels; ++ch) {
				auto *ring = samples.data() + ch * length + cursor;
				auto *plane = reinterpret_cast<float *>(audio->data[ch]);
				if (plane)
					std::swap_ranges(ring, ring + n, plane + offset);
				else
					std::fill_n(ring, n, 0.0f);
			}
			offset += n;
			cursor = (cursor + n) % length;
		}
	}
};

struct Filter {
	obs_source_t *context;
	std::array<std::atomic<obs_hotkey_id>, command_count> keys;
	std::mutex mutex;
	std::deque<Interval> ranges;
	uint64_t delay = 10 * second, history = 5 * second, generation = 1;
	uint64_t video_generation = 0;
	uint64_t buffered_ns = 0, last_render_ns = 0;
	PictureState picture = PictureState::waiting;
	std::string image_path;
	bool image_dirty = true;
	int strength = default_strength;
	VideoBuffer video;
	AudioBuffer audio;
	explicit Filter(obs_source_t *source) : context(source)
	{
		for (auto &id : keys)
			id.store(OBS_INVALID_HOTKEY_ID);
	}
};

void change_mark(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	auto &f = *static_cast<Filter *>(data);
	if (!pressed || (id == f.keys[toggle_mark] && !obs_source_enabled(f.context)))
		return;
	std::lock_guard lock(f.mutex);
	const bool marking = !f.ranges.empty() && f.ranges.back().end == open_end;
	if ((id == f.keys[set_mark] && marking) || (id == f.keys[cancel_mark] && !marking))
		return;
	const uint64_t now = os_gettime_ns();
	if (marking) {
		f.ranges.back().end = now;
		return;
	}
	uint64_t start = now > f.history ? now - f.history : 0;
	while (!f.ranges.empty() && f.ranges.back().end >= start) {
		start = std::min(start, f.ranges.back().start);
		f.ranges.pop_back();
	}
	f.ranges.push_back({start, open_end});
}

void attach(void *data, obs_source_t *parent)
{
	auto &f = *static_cast<Filter *>(data);
	// OBS filters are private sources. Hotkeys must belong to the visible parent.
	auto *settings = obs_source_get_settings(f.context);
	for (size_t i = 0; i < f.keys.size(); ++i) {
		const auto &spec = commands[i];
		const std::string name = std::string(spec.prefix) + obs_source_get_uuid(f.context);
		const auto id = obs_hotkey_register_source(parent, name.c_str(), obs_module_text(spec.label), change_mark, &f);
		f.keys[i] = id;
		if (id == OBS_INVALID_HOTKEY_ID)
			continue;
		if (obs_data_has_user_value(settings, spec.setting)) {
			auto *keys = obs_data_get_array(settings, spec.setting);
			obs_hotkey_load(id, keys);
			obs_data_array_release(keys);
		} else if (i == toggle_mark) {
			obs_key_combination_t key{};
			key.key = OBS_KEY_F2;
			obs_hotkey_load_bindings(id, &key, 1);
		}
	}
	obs_data_release(settings);
}

void save(void *data, obs_data_t *settings)
{
	auto &f = *static_cast<Filter *>(data);
	for (size_t i = 0; i < f.keys.size(); ++i) {
		const auto id = f.keys[i].load();
		if (id != OBS_INVALID_HOTKEY_ID) {
			auto *bindings = obs_hotkey_save(id);
			obs_data_set_array(settings, commands[i].setting, bindings);
			obs_data_array_release(bindings);
		}
	}
}

void detach(void *data, obs_source_t *)
{
	auto &f = *static_cast<Filter *>(data);
	auto *settings = obs_source_get_settings(f.context);
	save(data, settings);
	obs_data_release(settings);
	for (auto &key : f.keys) {
		const auto id = key.load();
		if (id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(id);
			key = OBS_INVALID_HOTKEY_ID;
		}
	}
}

double seconds_value(obs_data_t *s, const char *key)
{
	const double value = obs_data_get_double(s, key);
	return std::isfinite(value) ? std::clamp(value, 0.0, double(max_seconds)) : 0.0;
}

void update(void *data, obs_data_t *s)
{
	auto &f = *static_cast<Filter *>(data);
	std::lock_guard lock(f.mutex);
	const auto delay = uint64_t(std::llround(seconds_value(s, "delay") * second));
	if (f.delay != delay) {
		f.delay = delay;
		++f.generation;
		f.last_render_ns = 0;
	}
	f.history = std::min(delay, uint64_t(std::llround(seconds_value(s, "history") * second)));
	f.strength = int(std::clamp(obs_data_get_int(s, "strength"), 0LL, 100LL));
	const char *path = obs_data_get_string(s, "image");
	if (f.image_path != path) {
		f.image_path = path;
		f.image_dirty = true;
	}
}

void enabled(void *data, calldata_t *)
{
	auto &f = *static_cast<Filter *>(data);
	std::lock_guard lock(f.mutex);
	++f.generation;
	f.last_render_ns = 0;
}

void read_status(void *data, calldata_t *out)
{
	auto &f = *static_cast<Filter *>(data);
	const bool active = obs_source_enabled(f.context);
	std::lock_guard lock(f.mutex);
	const uint64_t now = os_gettime_ns();
	const bool recent = f.last_render_ns && now >= f.last_render_ns && now - f.last_render_ns < second;
	const auto picture = !active ? PictureState::bypass : recent ? f.picture : PictureState::waiting;
	calldata_set_int(out, "picture", static_cast<int>(picture));
	calldata_set_bool(out, "marking", active && !f.ranges.empty() && f.ranges.back().end == open_end);
	calldata_set_int(out, "buffered_ns", recent && active ? f.buffered_ns : 0);
	calldata_set_int(out, "delay_ns", f.delay);
	calldata_set_int(out, "history_ns", f.history);
	calldata_set_int(out, "strength", f.strength);
	calldata_set_string(out, "image", f.image_path.c_str());
	calldata_set_int(out, "key", static_cast<long long>(f.keys[toggle_mark].load()));
	calldata_set_int(out, "set_key", static_cast<long long>(f.keys[set_mark].load()));
	calldata_set_int(out, "cancel_key", static_cast<long long>(f.keys[cancel_mark].load()));
}

void render(void *data, gs_effect_t *)
{
	auto &f = *static_cast<Filter *>(data);
	uint64_t delay, generation;
	int strength;
	std::string path;
	bool reload;
	{
		std::lock_guard lock(f.mutex);
		delay = f.delay;
		generation = f.generation;
		strength = f.strength;
		reload = f.image_dirty;
		if (reload) {
			path = f.image_path;
			f.image_dirty = false;
		}
	}
	if (reload)
		f.video.set_image(path);
	if (f.video_generation != generation) {
		f.video.reset();
		f.video_generation = generation;
	}
	f.video.advance(f.context, obs_get_video_frame_time(), delay);
	const auto ts = f.video.displayed_time();
	bool covered;
	{
		std::lock_guard lock(f.mutex);
		while (!f.ranges.empty() && f.ranges.front().end <= ts)
			f.ranges.pop_front();
		covered = !f.ranges.empty() && ts >= f.ranges.front().start;
	}
	const auto picture = f.video.draw(covered, strength);
	{
		std::lock_guard lock(f.mutex);
		if (generation == f.generation) {
			f.picture = picture;
			f.buffered_ns = f.video.buffered_time();
			f.last_render_ns = os_gettime_ns();
		}
	}
}

obs_audio_data *filter_audio(void *data, obs_audio_data *audio)
{
	auto &f = *static_cast<Filter *>(data);
	uint64_t delay, generation;
	{
		std::lock_guard lock(f.mutex);
		delay = f.delay;
		generation = f.generation;
	}
	f.audio.process(audio, delay, generation);
	return audio;
}

void defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, "delay", 10);
	obs_data_set_default_double(s, "history", 5);
	obs_data_set_default_int(s, "strength", default_strength);
}

bool limit_history(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const double delay = seconds_value(s, "delay");
	obs_property_float_set_limits(obs_properties_get(props, "history"), 0, delay, 0.1);
	if (obs_data_get_double(s, "history") > delay)
		obs_data_set_double(s, "history", delay);
	return true;
}

bool clear_image(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<Filter *>(data);
	if (!f)
		return false;
	auto *change = obs_data_create();
	obs_data_set_string(change, "image", "");
	obs_source_update(f->context, change);
	obs_data_release(change);
	return true; // Refresh the open file picker with the cleared source setting.
}

obs_properties_t *properties(void *)
{
	auto *p = obs_properties_create();
	auto *delay = obs_properties_add_float(p, "delay", obs_module_text("Delay"), 0, max_seconds, 0.1);
	auto *history = obs_properties_add_float(p, "history", obs_module_text("History"), 0, max_seconds, 0.1);
	obs_property_float_set_suffix(delay, " s");
	obs_property_float_set_suffix(history, " s");
	obs_property_set_modified_callback(delay, limit_history);
	obs_properties_add_int_slider(p, "strength", obs_module_text("Strength"), 0, 100, 1);
	obs_properties_add_path(p, "image", obs_module_text("Image"), OBS_PATH_FILE,
				"Images (*.png *.jpg *.jpeg *.bmp)", nullptr);
	obs_properties_add_button(p, "clear_image", obs_module_text("ClearImage"), clear_image);
	obs_properties_add_button(p, "show_indicator", obs_module_text("ShowIndicator"),
				  [](obs_properties_t *, obs_property_t *, void *) {
					  indicator_show();
					  return false;
				  });
	return p;
}

void *create(obs_data_t *settings, obs_source_t *context)
{
	auto *f = new Filter(context);
	update(f, settings);
	obs_enter_graphics();
	f->video.init();
	obs_leave_graphics();
	signal_handler_connect(obs_source_get_signal_handler(context), "enable", enabled, f);
	proc_handler_add(obs_source_get_proc_handler(context),
			 "void censor_status(out int picture, out bool marking, out int buffered_ns, out int delay_ns, "
			 "out int history_ns, out int strength, out string image, out int key, out int set_key, "
			 "out int cancel_key)", read_status, f);
	return f;
}

void destroy(void *data)
{
	auto *f = static_cast<Filter *>(data);
	signal_handler_disconnect(obs_source_get_signal_handler(f->context), "enable", enabled, f);
	detach(f, nullptr);
	obs_enter_graphics();
	f->video.release();
	obs_leave_graphics();
	delete f;
}
} // namespace

bool obs_module_load()
{
	obs_source_info info{};
	info.id = "5050_censor_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO;
	info.get_name = [](void *) { return obs_module_text("Name"); };
	info.create = create;
	info.destroy = destroy;
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.update = update;
	info.filter_add = attach;
	info.filter_remove = detach;
	info.save = save;
	info.video_render = render;
	info.filter_audio = filter_audio;
	obs_register_source(&info);
	return true;
}

void obs_module_post_load()
{
	indicator_load();
}

void obs_module_unload()
{
	indicator_unload();
}
