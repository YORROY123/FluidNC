#pragma once

#include <atomic>
#include <cstdint>

namespace WebUI {
    // Timing out reports a stuck operation; it never releases ownership.
    // Only its worker may complete it, even if the caller has stopped waiting.
    class EthSingleFlight {
        enum State : uint8_t { Idle, Reserved, Ready, Running };
        std::atomic<State> _state { Idle };
        std::atomic<uint32_t> _started { 0 };
        bool _manual = false;
    public:
        bool submit(bool manual, uint32_t now) {
            State expected = Idle;
            if (!_state.compare_exchange_strong(expected, Reserved)) return false;
            _manual = manual;
            _started.store(now);
            _state.store(Ready, std::memory_order_release);
            return true;
        }
        bool take(bool& manual) {
            State expected = Ready;
            if (!_state.compare_exchange_strong(expected, Running, std::memory_order_acquire)) return false;
            manual = _manual;
            return true;
        }
        bool busy() const { return _state.load(std::memory_order_acquire) != Idle; }
        bool timedOut(uint32_t now, uint32_t timeout) const {
            State state = _state.load(std::memory_order_acquire);
            return (state == Ready || state == Running) && uint32_t(now - _started.load()) >= timeout;
        }
        void complete() { _state.store(Idle, std::memory_order_release); }
    };
}
