// Wrapper to include the wayland-scanner generated layer-shell header in C++.
// The generated code uses 'namespace' as a parameter name, which is reserved in C++.
// We rename it via a macro before inclusion, then undefine.
#pragma once
#ifdef __cplusplus
extern "C" {
#define namespace ns_layer
#include "wlr-layer-shell-unstable-v1-client.h"
#undef namespace
}
#else
#include "wlr-layer-shell-unstable-v1-client.h"
#endif
