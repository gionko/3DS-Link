#pragma once
#include <cstdio>
#include <cstdlib>
#define ASSERT(cond) do { if (!(cond)) { std::fprintf(stderr, "ASSERT failed: %s\n", #cond); std::abort(); } } while (0)
#define ASSERT_MSG(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "ASSERT_MSG failed: %s\n", #cond); std::abort(); } } while (0)
#define UNREACHABLE() std::abort()
