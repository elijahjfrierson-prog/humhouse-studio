#import <AVFoundation/AVFoundation.h>
#include <atomic>

// speakerOut = the phone plays through its own speaker / earpiece (no headphones): monitoring the mic there feeds back.
// Set now and on every route change, on the thread that posts it, so an unplug mid-take never monitors into the speaker.
static void hhsUpdateRoute (std::atomic<bool>* flag)
{
    bool spk = false;
    for (AVAudioSessionPortDescription* p in [AVAudioSession sharedInstance].currentRoute.outputs)
        if ([p.portType isEqualToString: AVAudioSessionPortBuiltInSpeaker] || [p.portType isEqualToString: AVAudioSessionPortBuiltInReceiver])
            spk = true;
    flag->store (spk);
}

void* hhsWatchPhoneRoute (std::atomic<bool>& speakerOut)
{
    std::atomic<bool>* flag = &speakerOut;
    hhsUpdateRoute (flag);
    id token = [[NSNotificationCenter defaultCenter] addObserverForName: AVAudioSessionRouteChangeNotification object: nil queue: nil
                                                             usingBlock: ^(NSNotification*) { hhsUpdateRoute (flag); }];
    return (__bridge_retained void*) token;
}

void hhsUnwatchPhoneRoute (void* token)   // before the engine goes away
{
    if (token == nullptr) return;
    id t = (__bridge_transfer id) token;
    [[NSNotificationCenter defaultCenter] removeObserver: t];
}

// a folder picked in Files lives outside the app and JUCE drops access to it right after the pick:
// reopen it from its bookmark and copy it into the app (kits / sample folders then always load, offline too)
namespace hhs { bool hhsCopyPickedFolder (void* bookmark, const char* urlString, const char* destPath); }   // declared in UI.h inside namespace hhs
bool hhs::hhsCopyPickedFolder (void* bookmark, const char* urlString, const char* destPath)
{
    NSURL* url = nil;
    if (bookmark != nullptr)
    {
        BOOL stale = NO;
        url = [NSURL URLByResolvingBookmarkData: (NSData*) bookmark options: 0 relativeToURL: nil bookmarkDataIsStale: &stale error: nil];
    }
    if (url == nil) url = [NSURL URLWithString: [NSString stringWithUTF8String: urlString]];
    if (url == nil) return false;
    const BOOL scoped = [url startAccessingSecurityScopedResource];
    NSFileManager* fm = [NSFileManager defaultManager];
    NSURL* dest = [NSURL fileURLWithPath: [NSString stringWithUTF8String: destPath]];
    [fm removeItemAtURL: dest error: nil];
    [fm createDirectoryAtURL: [dest URLByDeletingLastPathComponent] withIntermediateDirectories: YES attributes: nil error: nil];
    __block BOOL ok = NO;
    NSError* err = nil;
    NSFileCoordinator* fc = [[NSFileCoordinator alloc] initWithFilePresenter: nil];
    [fc coordinateReadingItemAtURL: url options: NSFileCoordinatorReadingWithoutChanges error: &err byAccessor: ^(NSURL* u) {
        ok = [fm copyItemAtURL: u toURL: dest error: nil];
    }];
   #if ! __has_feature(objc_arc)
    [fc release];
   #endif
    if (scoped) [url stopAccessingSecurityScopedResource];
    return ok;
}
