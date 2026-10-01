#pragma once

#include <sdbus-c++/sdbus-c++.h>

// Backend methods are meant to be called by xdg-desktop-portal only. The frontend
// checks that an app only touches its own sessions; a direct caller would skip
// that and could drive another app's session. Throws AccessDenied unless the
// message being processed by object comes from the frontend.
void requireFrontendCaller(const sdbus::IObject& object, const char* method);
