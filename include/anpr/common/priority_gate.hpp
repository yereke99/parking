#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace anpr {

/// Admits one thread at a time to a shared resource, highest priority first and in arrival order
/// within a priority.
///
/// Several cameras share one TensorRT detector. With a plain mutex, cameras watching an empty
/// lane take as many GPU turns as the one camera that has a car stopped at its barrier, and on a
/// 4 GB Nano that camera then sees a quarter of the detector rate exactly when the multi-frame
/// vote needs it most. Whoever waits with the higher priority goes next; a camera only competes
/// while it is actually waiting, so idle cameras still get every turn nobody more urgent wants.
class PriorityGate {
public:
    void acquire(int priority);
    void release();

    /// Holds the gate for one scope.
    class Scoped {
    public:
        Scoped(PriorityGate& gate, int priority) : gate_(gate) { gate_.acquire(priority); }
        ~Scoped() { gate_.release(); }
        Scoped(const Scoped&) = delete;
        Scoped& operator=(const Scoped&) = delete;

    private:
        PriorityGate& gate_;
    };

private:
    struct Waiter {
        int priority;
        std::uint64_t ticket;
    };

    std::mutex mutex_;
    std::condition_variable changed_;
    bool busy_{false};
    std::uint64_t next_ticket_{0};
    std::vector<Waiter> waiters_;

    [[nodiscard]] bool isNext(std::uint64_t ticket) const;
};

}  // namespace anpr
