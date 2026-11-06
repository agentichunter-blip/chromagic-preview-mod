#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "box_art_format.h"
/* CMAR v1: fixed size, little-endian RGB565, no compression or transparency. */
bool BoxArt_Read(FILE *File, uint8_t Pixels[BOX_ART_BYTES]);
