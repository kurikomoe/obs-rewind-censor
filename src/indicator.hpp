#pragma once

enum class PictureState { waiting, buffering, clear, mosaic, image, black, bypass };

void indicator_load();
void indicator_show();
void indicator_unload();
