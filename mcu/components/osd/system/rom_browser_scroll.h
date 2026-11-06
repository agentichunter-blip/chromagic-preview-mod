#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { kScrollUp = 1, kScrollDown = 2 };
typedef struct {
    uint64_t Started, Last;
    uint32_t Credit;
    int Direction;
    unsigned Rate;
    bool RequireRelease;
} RomBrowserScroll_t;
typedef struct { int Steps; bool Wrap; } RomBrowserScrollStep_t;

void RomBrowserScroll_Reset(RomBrowserScroll_t *Scroll, unsigned Held);
RomBrowserScrollStep_t RomBrowserScroll_Update(RomBrowserScroll_t *Scroll,
    uint64_t NowMs, unsigned Held, unsigned Pressed);
