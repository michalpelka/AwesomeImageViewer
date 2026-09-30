#pragma once
#include <string>
#include <vector>

// macOS integration that GLFW (used by raylib) does not provide. No-ops elsewhere.

// Must be called BEFORE InitWindow: makes Finder "Open" / "Open With" / `open -a` deliver
// files to the app (they arrive as Apple Events, not argv).
void macInstallOpenHandler();
// Files opened via Finder since the last call.
std::vector<std::string> macTakeOpenedFiles();

// Trackpad pinch: GLFW ignores NSEventTypeMagnify. Call after InitWindow.
void macInstallGestures();
// Magnification since the last call (e.g. 0.05 = zoom in 5%), then resets to 0.
float macConsumeMagnify();
