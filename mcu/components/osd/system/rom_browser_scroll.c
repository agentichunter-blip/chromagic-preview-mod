#include "rom_browser_scroll.h"

void RomBrowserScroll_Reset(RomBrowserScroll_t *Scroll, unsigned Held)
{
    *Scroll = (RomBrowserScroll_t){ .RequireRelease = Held != 0 };
}

static unsigned RateAt(uint64_t Age)
{
    return Age < 350 ? 0 : Age < 1200 ? 5 : Age < 2500 ? 12 : Age < 4000 ? 30 : 60;
}

/* Integral in thousandths of a step, including the first repeat at 350 ms. */
static uint64_t Accumulated(uint64_t Age)
{
    const unsigned Ends[] = {350, 1200, 2500, 4000};
    const unsigned Rates[] = {5, 12, 30, 60};
    uint64_t Result = Age >= 350 ? 1000 : 0;
    for (unsigned I = 0; I < 4; ++I)
    {
        if (Age <= Ends[I]) break;
        const uint64_t End = I < 3 && Age > Ends[I + 1] ? Ends[I + 1] : Age;
        Result += (End - Ends[I]) * Rates[I];
    }
    return Result;
}

RomBrowserScrollStep_t RomBrowserScroll_Update(RomBrowserScroll_t *S,
    uint64_t Now, unsigned Held, unsigned Pressed)
{
    RomBrowserScrollStep_t Result = {0};
    Held &= 3; Pressed &= 3;
    if (S->RequireRelease)
    {
        if (!Held) S->RequireRelease = false;
        return Result;
    }
    if (Held == 3 || Pressed == 3)
    {
        *S = (RomBrowserScroll_t){0};
        return Result;
    }
    const unsigned Input = Held ? Held : Pressed;
    const int Direction = Input == kScrollUp ? -1 : Input == kScrollDown ? 1 : 0;
    if (!Direction)
    {
        *S = (RomBrowserScroll_t){0};
        return Result;
    }
    if (Direction != S->Direction || Pressed)
    {
        *S = (RomBrowserScroll_t){ .Started = Now, .Last = Now, .Direction = Held ? Direction : 0 };
        return (RomBrowserScrollStep_t){ .Steps = Direction, .Wrap = Pressed != 0 };
    }
    if (Now < S->Last) { RomBrowserScroll_Reset(S, Held); return Result; }
    uint64_t Previous = S->Last;
    if (Now - Previous > 60) Previous = Now - 60; /* Bounded catch-up after a stall. */
    const uint64_t Age = Now - S->Started;
    S->Credit += (uint32_t)(Accumulated(Age) - Accumulated(Previous - S->Started));
    S->Last = Now;
    S->Rate = RateAt(Age);
    unsigned Steps = S->Credit / 1000;
    S->Credit %= 1000;
    if (Steps > 4) Steps = 4;
    Result.Steps = Direction * (int)Steps;
    return Result;
}
