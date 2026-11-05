#include <atomic>
std::atomic<long> work_calls{0};
int work() {
    work_calls.fetch_add(1, std::memory_order_relaxed);
    return 2;
}
