#pragma once
#include <windows.h>

// The same executable is a normal Win32 installer when double-clicked,
// and a Native Messaging host when Chrome launches it with an origin.
int RunSetup(bool silent);
