#pragma once
#include <functional>

namespace aligna
{
// Asks the OS for microphone access (macOS: AVFoundation authorization; prompts only if the user has not
// decided yet). `callback(granted)` runs on the message thread, possibly before this returns.
// Other platforms: no permission to ask for, always granted.
void requestMicrophoneAccess(std::function<void(bool granted)> callback);
} // namespace aligna
