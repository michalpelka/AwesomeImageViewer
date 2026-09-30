#include "mac_platform.hpp"

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>
#include <utility>

namespace {
// Everything below runs on the main thread (AppKit event dispatch).
double g_magnify = 0;
id g_monitor = nil;
std::vector<std::string> g_opened;

void openURLs(id, SEL, NSApplication*, NSArray<NSURL*>* urls)
{
    for (NSURL* u in urls)
        if (u.isFileURL) g_opened.emplace_back(u.path.fileSystemRepresentation);
}
}  // namespace

void macInstallOpenHandler()
{
    // GLFW installs its own NSApplicationDelegate during glfwInit and the launch-time
    // "open documents" event is dispatched right then, so the method has to exist on the
    // delegate class beforehand. GLFW doesn't implement it, so adding it is safe.
    Class cls = NSClassFromString(@"GLFWApplicationDelegate");
    if (cls) class_addMethod(cls, @selector(application:openURLs:), reinterpret_cast<IMP>(openURLs), "v@:@@");
}

std::vector<std::string> macTakeOpenedFiles()
{
    return std::exchange(g_opened, {});
}

void macInstallGestures()
{
    if (g_monitor) return;
    g_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
                                                      handler:^NSEvent*(NSEvent* e) {
                                                          g_magnify += e.magnification;
                                                          return e;
                                                      }];
}

float macConsumeMagnify()
{
    return float(std::exchange(g_magnify, 0.0));
}
