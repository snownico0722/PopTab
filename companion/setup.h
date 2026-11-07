#pragma once
#include <filesystem>
// The same EXE installs/repairs, runs the tray, and serves Native Messaging.
int RunSetup(bool silent, bool uninstall = false);
bool StopHostsForCompanion(const std::filesystem::path& installed);
