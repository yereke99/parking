#include "anpr/common/priority_gate.hpp"

#include <algorithm>

namespace anpr {

bool PriorityGate::isNext(std::uint64_t ticket) const {
    const auto best = std::min_element(
        waiters_.begin(), waiters_.end(), [](const Waiter& lhs, const Waiter& rhs) {
            return lhs.priority != rhs.priority ? lhs.priority > rhs.priority
                                                : lhs.ticket < rhs.ticket;
        });
    return best != waiters_.end() && best->ticket == ticket;
}

void PriorityGate::acquire(int priority) {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t ticket = next_ticket_++;
    waiters_.push_back(Waiter{priority, ticket});
    changed_.wait(lock, [this, ticket] { return !busy_ && isNext(ticket); });
    waiters_.erase(std::find_if(waiters_.begin(), waiters_.end(),
                                [ticket](const Waiter& waiter) { return waiter.ticket == ticket; }));
    busy_ = true;
}

void PriorityGate::release() {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        busy_ = false;
    }
    // Every waiter re-checks whether it is now first; there are at most a handful (one per
    // camera), so waking all of them costs nothing.
    changed_.notify_all();
}

}  // namespace anpr
