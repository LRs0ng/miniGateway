#pragma once

// The loader intentionally keeps the boundary to two C symbols. The object
// itself still implements the normal C++ gateway interface, but construction
// and destruction never cross the module boundary as C++ allocation calls.
#if defined(_WIN32)
#define GATEWAY_PLUGIN_EXPORT __declspec(dllexport)
#else
#define GATEWAY_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define GATEWAY_PLUGIN_C extern "C"

namespace gateway {

using CreatePluginFn = void* (*)(const char* settings_json);
using DestroyPluginFn = void (*)(void* plugin);

}  // namespace gateway

