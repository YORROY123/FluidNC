#include "Driver/restart.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"

void restart() {
    esp_restart();
    while (1) {}
}

bool restart_was_panic() {
    return esp_reset_reason() == ESP_RST_PANIC;
}

namespace {
    // RTC slow memory survives a panic reset but comes up as garbage after a
    // power cycle, hence the magic number.
    constexpr uint32_t kStreakMagic = 0x53545250;  // "PRTS"
    RTC_NOINIT_ATTR uint32_t _streak_magic;
    RTC_NOINIT_ATTR uint32_t _streak;

    // A board that stays up this long was not in a crash loop, so its next
    // panic starts a new streak instead of extending this one.
    constexpr uint64_t kStableUs = 60ULL * 1000 * 1000;

    void streak_ended(void*) {
        _streak = 0;
    }
}

uint32_t restart_panic_streak() {
    static bool     computed = false;
    static uint32_t streak   = 0;
    if (computed) {
        return streak;
    }
    computed = true;

    if (restart_was_panic()) {
        uint32_t previous = (_streak_magic == kStreakMagic) ? _streak : 0;
        streak            = (previous < 1000 ? previous : 1000) + 1;
    }
    _streak_magic = kStreakMagic;
    _streak       = streak;

    if (streak) {
        esp_timer_create_args_t args = {};
        args.callback                = streak_ended;
        args.name                    = "panic_streak";
        esp_timer_handle_t timer;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, kStableUs);
        }
    }
    return streak;
}
