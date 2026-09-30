#include "mac_gestures.hpp"

#import <Cocoa/Cocoa.h>

namespace {
double g_magnify = 0;  // only touched on the main thread
id g_monitor = nil;
}  // namespace

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
    const double m = g_magnify;
    g_magnify = 0;
    return float(m);
}
