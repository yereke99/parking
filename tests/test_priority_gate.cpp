#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "anpr/common/priority_gate.hpp"

#include "test_framework.hpp"

namespace {

/// Waits (bounded) until `count` threads have queued behind the holder.
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

}  // namespace

TEST("priority gate serves the highest waiting priority first, FIFO within one") {
    anpr::PriorityGate gate;
    std::mutex order_mutex;
    std::vector<int> order;
    gate.acquire(0);  // the holder: every thread below queues behind it

    std::vector<std::thread> threads;
    const int priorities[] = {0, 3, 1, 3, 2};
    for (int index = 0; index < 5; ++index) {
        threads.emplace_back([&, index] {
            gate.acquire(priorities[index]);
            {
                const std::lock_guard<std::mutex> guard(order_mutex);
                order.push_back(index);
            }
            gate.release();
        });
        settle();  // fixes the arrival order
    }
    gate.release();
    for (std::thread& thread : threads) {
        thread.join();
    }
    // Priority 3 (indices 1 then 3 in arrival order), then 2, 1, 0.
    const std::vector<int> expected{1, 3, 4, 2, 0};
    CHECK(order == expected);
}

TEST("priority gate admits one holder at a time and never loses a waiter") {
    anpr::PriorityGate gate;
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&, index] {
            for (int round = 0; round < 200; ++round) {
                const anpr::PriorityGate::Scoped turn(gate, (index + round) % 4);
                const int now = ++inside;
                int seen = max_inside.load();
                while (now > seen && !max_inside.compare_exchange_weak(seen, now)) {
                }
                --inside;
            }
            ++done;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(done.load(), 8);
    CHECK_EQ(max_inside.load(), 1);
}
