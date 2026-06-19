// Owner-drawn progress window.
#pragma once

#include <string>

namespace progress {

// Create and show the window (no-op if already shown).
void show(const std::wstring& title);

// Set the status line.
void set_status(const std::wstring& text);

// 0..100 for a determinate bar; -1 for indeterminate.
void set_progress(int percent);

// Process pending window messages.
void pump();

// Destroy the window.
void close();

} // namespace progress
