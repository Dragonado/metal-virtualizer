// Emits metal-cpp's Foundation implementation (selector/class symbols) exactly
// once, so NS::String / NS::Error work locally in every consumer.
//
// MTL_PRIVATE_IMPLEMENTATION is deliberately absent: with no Metal
// implementation in client binaries, any client call that escapes the shim to
// real local Metal fails at link time instead of silently running on the
// local GPU. The server emits it in its own TU (server.cpp) — keep it defined
// in exactly one TU per binary or the linker reports duplicate symbols.
#define NS_PRIVATE_IMPLEMENTATION

#include "Foundation/Foundation.hpp"
