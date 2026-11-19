#import <AVFoundation/AVFoundation.h>

// true when the phone plays through its own speaker / earpiece (no headphones): monitoring the mic there feeds back
bool hhsPhoneSpeakerOut()
{
    for (AVAudioSessionPortDescription* p in [AVAudioSession sharedInstance].currentRoute.outputs)
        if ([p.portType isEqualToString: AVAudioSessionPortBuiltInSpeaker] || [p.portType isEqualToString: AVAudioSessionPortBuiltInReceiver])
            return true;
    return false;
}
