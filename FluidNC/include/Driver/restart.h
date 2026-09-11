#include <cstdint>

bool restart_was_panic();
void restart();

// How many restarts in a row, including this one, were caused by a panic:
// 0 when the last restart was anything else (power on, reset button, $Bye).
// A platform that cannot count them returns UINT32_MAX after a panic, which
// callers treat as the most conservative case.
uint32_t restart_panic_streak();
