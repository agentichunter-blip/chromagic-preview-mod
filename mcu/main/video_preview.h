#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include "preview_format.h"
typedef struct { FILE *File; uint32_t Count, Index; uint16_t FPS; } VideoPreview_t;
/* Caller owns FILE and storage. Pixels must be 4-byte aligned and can be
 * ESP32 32-bit-only instruction RAM; all pixel accesses are whole words. */
void Preview_CopyToBytes(uint8_t *Output, const uint8_t *Pixels);
bool VideoPreview_Open(VideoPreview_t *Video, FILE *File);
bool VideoPreview_Read(VideoPreview_t *Video, uint8_t Pixels[PREVIEW_BYTES]);
bool PanelArt_Read(FILE *File, uint8_t Pixels[PREVIEW_BYTES]);
