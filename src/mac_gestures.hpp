#pragma once

// Trackpad pinch support. GLFW (used by raylib) ignores NSEventTypeMagnify, so on macOS
// a local Cocoa event monitor accumulates it. No-ops on other platforms.
void macInstallGestures();
// Magnification since the last call (e.g. 0.05 = zoom in 5%), then resets to 0.
float macConsumeMagnify();
