#include "app/MicPermission.h"
#include <juce_events/juce_events.h>
#import <AVFoundation/AVFoundation.h>

namespace aligna
{
void requestMicrophoneAccess(std::function<void(bool granted)> callback)
{
    if (!callback)
        return;
    if (@available(macOS 10.14, *))
    {
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
        {
            case AVAuthorizationStatusAuthorized:
                callback(true);
                return;
            case AVAuthorizationStatusNotDetermined:
            {
                // The block keeps its own copy of the callback; the answer arrives on an arbitrary queue.
                const std::function<void(bool)> cb = std::move(callback);
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                         completionHandler:^(BOOL granted) {
                                             const bool ok = granted == YES;
                                             juce::MessageManager::callAsync([cb, ok] { cb(ok); });
                                         }];
                return;
            }
            case AVAuthorizationStatusDenied:
            case AVAuthorizationStatusRestricted:
                callback(false);
                return;
        }
        callback(false); // a status this code does not know
        return;
    }
    callback(true); // before macOS 10.14 there is no microphone permission
}
} // namespace aligna
