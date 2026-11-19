#import <AVFoundation/AVFoundation.h>
#include <atomic>

// speakerOut = the phone plays through its own speaker / earpiece (no headphones): monitoring the mic there feeds back.
// Set now and on every route change, on the thread that posts it, so an unplug mid-take never monitors into the speaker.
void* hhsWatchPhoneRoute (std::atomic<bool>& speakerOut)
{
    std::atomic<bool>* flag = &speakerOut;
    void (^update)(void) = ^{
        bool spk = false;
        for (AVAudioSessionPortDescription* p in [AVAudioSession sharedInstance].currentRoute.outputs)
            if ([p.portType isEqualToString: AVAudioSessionPortBuiltInSpeaker] || [p.portType isEqualToString: AVAudioSessionPortBuiltInReceiver])
                spk = true;
        flag->store (spk);
    };
    update();
    id token = [[NSNotificationCenter defaultCenter] addObserverForName: AVAudioSessionRouteChangeNotification object: nil queue: nil
                                                             usingBlock: ^(NSNotification*) { update(); }];
    return (__bridge_retained void*) token;
}

void hhsUnwatchPhoneRoute (void* token)   // before the engine goes away
{
    if (token == nullptr) return;
    id t = (__bridge_transfer id) token;
    [[NSNotificationCenter defaultCenter] removeObserver: t];
}
