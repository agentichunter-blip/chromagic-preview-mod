#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Display and sorting share this normalization; disk filenames stay untouched. */
void RomTitle_Make(const char *Name, bool Directory, char *Output, size_t Size);
int RomTitle_Compare(const char *Left, const char *Right, bool Directory);
const char *RomTitle_Group(const char *Title, bool Directory, char Letter[2]);
