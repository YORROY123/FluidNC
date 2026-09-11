#include "EthSingleFlight.h"
#include <cassert>
#include <thread>
#include <atomic>
#include <vector>

int main() {
    WebUI::EthSingleFlight flight;
    bool manual = false;
    assert(!flight.busy());
    assert(!flight.take(manual));
    assert(flight.submit(true, 100));
    assert(!flight.submit(false, 101));
    assert(flight.take(manual) && manual);
    assert(!flight.take(manual));
    assert(!flight.timedOut(15099, 15000));
    assert(flight.timedOut(15100, 15000));
    // Simulated permanently blocked driver: deadlines never allow overlap.
    for (unsigned now = 15100; now < 100000; now += 100) {
        assert(flight.timedOut(now, 15000));
        assert(!flight.submit(false, now));
    }
    // A late return safely releases the slot for the next attempt.
    flight.complete();
    assert(!flight.timedOut(100000, 15000));
    assert(flight.submit(false, 0xfffffff0));
    assert(flight.take(manual) && !manual);
    assert(!flight.timedOut(0x10, 33));
    assert(flight.timedOut(0x11, 33));
    flight.complete();
    // Simultaneous manual and automatic requests have exactly one winner.
    for (int round = 0; round < 100; ++round) {
        std::atomic<int> winners { 0 };
        std::atomic<bool> go { false };
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back([&, i] {
            while (!go.load()) std::this_thread::yield();
            if (flight.submit(i % 2, 1000)) ++winners;
        });
        go.store(true);
        for (auto& thread : threads) thread.join();
        assert(winners == 1);
        assert(flight.take(manual));
        flight.complete();
    }
}
