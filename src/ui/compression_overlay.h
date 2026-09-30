#pragma once
#include "types/types.h"
#include <cstdint>
namespace compression {
void menu(const Terminal::Pair& pair, bool replay);
void render(const Terminal::Pair& pair, int64_t clock);
}
