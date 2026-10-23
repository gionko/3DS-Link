#include "ctr/teardown.hpp"

namespace {
volatile bool g_teardown = false;
} // namespace

// Every reference to free() in the linked objects (libctrpf, libstdc++'s operator delete, ours)
// ends up here thanks to -Wl,--wrap=free.
extern "C" void __real_free(void* p);
extern "C" void __wrap_free(void* p) {
    if (!g_teardown) {
        __real_free(p);
    }
}

namespace azl {
namespace ctr {

void BeginTeardown() {
    g_teardown = true;
}

bool InTeardown() {
    return g_teardown;
}

} // namespace ctr
} // namespace azl
