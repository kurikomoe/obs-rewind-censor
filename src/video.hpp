#pragma once

#include <obs-module.h>
#include "indicator.hpp"
#include <deque>
#include <string>

// Owned by the OBS graphics thread. All timestamps use the OBS monotonic clock.
class VideoBuffer
{
      public:
	void init();
	void release();
	void reset();
	void set_image(const std::string &path);
	void advance(obs_source_t *filter, uint64_t now, uint64_t delay);
	PictureState draw(bool covered, int strength);
	uint64_t buffered_time() const;
	uint64_t displayed_time() const
	{
		return displayed.time;
	}

      private:
	struct Frame {
		uint64_t time = 0;
		gs_texrender_t *render = nullptr;
	};
	bool capture(obs_source_t *filter, obs_source_t *target, gs_texrender_t *packed);
	void draw_packed(gs_texture_t *texture, const char *technique, int strength, uint32_t draw_width,
			 uint32_t draw_height);

	gs_effect_t *effect = nullptr;
	gs_texrender_t *scratch = nullptr;
	gs_texture_t *image = nullptr;
	std::string image_path;
	std::deque<Frame> pending;
	Frame displayed;
	uint32_t width = 0, height = 0;
	uint64_t last_tick = 0;
	bool capture_failed = false;
};
