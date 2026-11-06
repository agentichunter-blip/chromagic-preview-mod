#pragma once
/* Native panel frames; little endian RGB565. No decoder runs on the device. */
enum { PREVIEW_WIDTH = 128, PREVIEW_HEIGHT = 92,
       PREVIEW_BYTES = PREVIEW_WIDTH * PREVIEW_HEIGHT * 2,
       PREVIEW_HEADER_BYTES = 24, PREVIEW_MAX_FRAMES = 450 };
