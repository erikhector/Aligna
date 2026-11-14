#include "app/MicPermission.h"

namespace aligna
{
// Non-macOS: no microphone permission; whether an input exists is checked when it is opened.
void requestMicrophoneAccess(std::function<void(bool granted)> callback)
{
    if (callback)
        callback(true);
}
} // namespace aligna
