// Updater entry point.
#pragma once

#include <filesystem>

namespace updater {

// Run the update check/install. baseDir is the folder containing dwmapi.dll;
// config and install target are baseDir/"ue4ss".
void run_blocking(const std::filesystem::path& baseDir);

} // namespace updater
