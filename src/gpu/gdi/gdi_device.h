// GDI software fallback device — implements GpuDevice via GDI DIB.
//
// This is the simplest possible backend: a GDI window with a DIB section
// that the boot screen and, in a pinch, the guest framebuffer blit into.
// No 3D acceleration, no shaders — pure software presentation.
//
// Used as the last-resort fallback when Vulkan is unavailable, and for
// the boot-progress overlay before the GPU backend is ready.

#pragma once
#include "../gal.h"

// Create a GDI-backed GPU device (Windows only).
// Returns nullptr if window creation or DIB init fails.
GpuDevice* CreateGdiDevice(const GalConfig& config,
                            const GalWindowCallbacks& callbacks);
