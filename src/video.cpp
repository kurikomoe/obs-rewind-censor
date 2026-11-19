#include "video.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
void solid_black(uint32_t width, uint32_t height)
{
	auto *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 color;
	vec4_set(&color, 0, 0, 0, 1);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, width, height);
}

void set_size(gs_effect_t *effect, const char *name, uint32_t width, uint32_t height)
{
	vec2 value;
	vec2_set(&value, float(width), float(height));
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, name), &value);
}
} // namespace

void VideoBuffer::init()
{
	char *path = obs_module_file("censor.effect");
	char *errors = nullptr;
	if (path)
		effect = gs_effect_create_from_file(path, &errors);
	if (!effect)
		blog(LOG_ERROR, "[5050 Censor] Cannot load video shader: %s", errors ? errors : "file missing");
	bfree(errors);
	bfree(path);
	scratch = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
}

void VideoBuffer::reset()
{
	for (const auto &frame : pending)
		gs_texrender_destroy(frame.render);
	pending.clear();
	gs_texrender_destroy(displayed.render);
	displayed = {};
	last_tick = 0;
}

void VideoBuffer::release()
{
	reset();
	gs_texture_destroy(image);
	gs_effect_destroy(effect);
	gs_texrender_destroy(scratch);
}

void VideoBuffer::set_image(const std::string &path)
{
	if (image_path == path)
		return;
	image_path = path;
	gs_texture_destroy(image);
	image = path.empty() ? nullptr : gs_texture_create_from_file(path.c_str());
	if (!image && !path.empty())
		blog(LOG_WARNING, "[5050 Censor] Cannot read replacement image '%s', using black",
		     path.c_str());
}

void VideoBuffer::draw_packed(gs_texture_t *texture, const char *technique, int strength, uint32_t draw_width,
			      uint32_t draw_height)
{
	set_size(effect, "frame_size", width, height);
	set_size(effect, "packed_size", (width + 3) / 4, height + (height + 1) / 2);
	// Exponential spacing keeps fine and coarse adjustments useful. At 100
	// the entire source fits inside a single block.
	const float block =
	    strength == 0
		? 1.0f
		: std::max(2.0f, std::round(std::pow(float(std::max(width, height)), strength / 100.0f)));
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "block_size"), block);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(texture, 0, draw_width, draw_height);
}

bool VideoBuffer::capture(obs_source_t *filter, obs_source_t *target, gs_texrender_t *packed)
{
	gs_texrender_reset(scratch);
	if (!gs_texrender_begin(scratch, width, height))
		return false;
	gs_ortho(0, float(width), 0, float(height), -100, 100);
	vec4 black;
	vec4_set(&black, 0, 0, 0, 1);
	gs_clear(GS_CLEAR_COLOR, &black, 0, 0);
	const auto flags = obs_source_get_output_flags(target);
	if (target == obs_filter_get_parent(filter) && !(flags & (OBS_SOURCE_ASYNC | OBS_SOURCE_CUSTOM_DRAW)))
		obs_source_default_render(target);
	else
		obs_source_video_render(target);
	gs_texrender_end(scratch);

	const uint32_t pw = (width + 3) / 4, ph = height + (height + 1) / 2;
	gs_texrender_reset(packed);
	if (!gs_texrender_begin(packed, pw, ph))
		return false;
	gs_ortho(0, float(pw), 0, float(ph), -100, 100);
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(false); // Alpha holds a fourth byte of YUV data.
	draw_packed(gs_texrender_get_texture(scratch), "Pack", 0, pw, ph);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb);
	gs_texrender_end(packed);
	return true;
}

void VideoBuffer::advance(obs_source_t *filter, uint64_t now, uint64_t delay)
{
	if (now == last_tick)
		return; // Preview, projector and program may render the same tick.
	auto *target = obs_filter_get_target(filter);
	const auto w = target ? obs_source_get_base_width(target) : 0;
	const auto h = target ? obs_source_get_base_height(target) : 0;
	if (w != width || h != height || now < last_tick || (last_tick && now - last_tick > 1000000000ULL))
		reset();
	width = w;
	height = h;
	last_tick = now;
	if (!width || !height || !effect || !scratch)
		return;

	const uint64_t cutoff = now > delay ? now - delay : 0;
	gs_texrender_t *reuse = nullptr;
	while (!pending.empty() && pending.front().time <= cutoff) {
		gs_texrender_destroy(reuse);
		reuse = displayed.render;
		displayed = pending.front();
		pending.pop_front();
	}
	if (!delay)
		reuse = std::exchange(displayed.render, nullptr);
	if (!reuse)
		reuse = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const bool captured = reuse && capture(filter, target, reuse);
	if (!captured) {
		if (!capture_failed)
			blog(LOG_WARNING, "[5050 Censor] Frame capture failed, substituting black");
		gs_texrender_destroy(reuse);
		reuse = nullptr;
	}
	capture_failed = !captured;
	Frame frame{now, reuse};
	if (delay)
		pending.push_back(frame);
	else
		displayed = frame;
}

uint64_t VideoBuffer::buffered_time() const
{
	if (pending.empty())
		return 0;
	const uint64_t oldest = displayed.time ? displayed.time : pending.front().time;
	return pending.back().time - oldest;
}

PictureState VideoBuffer::draw(bool covered, int strength)
{
	if (!width || !height)
		return PictureState::waiting;
	PictureState state = PictureState::black;
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	if (displayed.time && covered && !image_path.empty()) {
		solid_black(width, height);
		if (image) {
			state = PictureState::image;
			const uint32_t iw = gs_texture_get_width(image), ih = gs_texture_get_height(image);
			const double scale = std::min(double(width) / iw, double(height) / ih);
			const auto w = std::clamp(uint32_t(std::lround(iw * scale)), 1u, width);
			const auto h = std::clamp(uint32_t(std::lround(ih * scale)), 1u, height);
			gs_matrix_push();
			gs_matrix_translate3f(float((width - w) / 2), float((height - h) / 2), 0);
			auto *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), image);
			gs_enable_blending(true);
			gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ZERO,
						   GS_BLEND_ONE);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(image, 0, w, h);
			gs_matrix_pop();
		}
	} else if (displayed.render) {
		state = covered && strength > 0 ? PictureState::mosaic : PictureState::clear;
		draw_packed(gs_texrender_get_texture(displayed.render), "Display", covered ? strength : 0,
			    width, height);
	} else {
		solid_black(width, height);
		if (!displayed.time && !pending.empty() && !capture_failed)
			state = PictureState::buffering;
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb);
	return state;
}
