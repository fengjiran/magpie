#include "watchdog.hpp"
int main() {
    magpie::test::Watchdog watchdog(
        [] { std::fputs("watchdog-probe pending=1 gate=1 task-id=17\n", stderr); },
        std::chrono::seconds(1));
    for (;;) {
        std::this_thread::yield();
    }
}
