#include "metal_shim.h"

// The Hijack: Replace the MTL namespace with your Shim namespace.
// Any code in adder.cpp below this point will use MetalShim instead of MTL.
#define MTL MetalShim