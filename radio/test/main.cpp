#include <cstdlib>
#include <cstring>

#include "test.h"

namespace t {
Case*& head() {
    static Case* h = nullptr;
    return h;
}
int& failures() {
    static int n = 0;
    return n;
}
static const char* current = "";
void fail(const char* file, int line, const std::string& what) {
    failures()++;
    if (failures() <= 200) std::printf("FAIL %s (%s:%d): %s\n", current, file, line, what.c_str());
}
std::string root() { return SRC_ROOT; }
}  // namespace t

int main(int argc, char** argv) {
    int run = 0;
    // Registration order is reversed; run in source order.
    t::Case* list[512];
    int n = 0;
    for (t::Case* c = t::head(); c && n < 512; c = c->next) list[n++] = c;
    for (int i = n - 1; i >= 0; i--) {
        if (argc > 1 && !std::strstr(list[i]->name, argv[1])) continue;
        t::current = list[i]->name;
        const int before = t::failures();
        list[i]->fn();
        run++;
        if (t::failures() != before) std::printf("  %s failed\n", list[i]->name);
    }
    std::printf("%d tests, %d failures\n", run, t::failures());
    return t::failures() ? 1 : 0;
}
