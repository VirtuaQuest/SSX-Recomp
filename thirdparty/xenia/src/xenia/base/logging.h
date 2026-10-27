// Shim: Xenia's logging routed to stderr.
#pragma once
#include <cstdio>
#include <fmt/format.h>

#define XELOGE(...) (fmt::print(stderr, "[xenia] " __VA_ARGS__), fmt::print(stderr, "\n"))
#define XELOGW(...) (fmt::print(stderr, "[xenia] " __VA_ARGS__), fmt::print(stderr, "\n"))
#define XELOGI(...) ((void)0)
#define XELOGD(...) ((void)0)
#define XELOGGPU(...) ((void)0)
