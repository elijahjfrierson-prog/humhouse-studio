#import <StoreKit/StoreKit.h>
#import <UIKit/UIKit.h>
#include <functional>
#include <string>

// App Store app: free = the stripped phone set; "HumHouse Pro" ($7.99/month auto-renewing) unlocks everything.
// Every purchase / renewal / restore extends "full until" to that transaction's date + 33 days, so a cancelled
// subscription locks again after its last paid month. Renewals arrive through the observer on launch.
static NSString* const hhsFullProduct = @"com.humhouse.studio.full.monthly";
static NSString* const hhsUntilKey = @"hhsFullUntil";
static std::function<void (bool, std::string)> hhsStoreChanged;

static bool hhsFullNow() { return [[NSUserDefaults standardUserDefaults] doubleForKey: hhsUntilKey] > [[NSDate date] timeIntervalSince1970]; }
static void hhsTell (std::string msg) { const bool full = hhsFullNow(); dispatch_async (dispatch_get_main_queue(), ^{ if (hhsStoreChanged) hhsStoreChanged (full, msg); }); }

@interface HHSStore : NSObject <SKPaymentTransactionObserver, SKProductsRequestDelegate>
@property (nonatomic, strong) SKProduct* product;
@property (nonatomic, strong) SKProductsRequest* request;
@property (nonatomic) BOOL buyWhenLoaded;
@end

@implementation HHSStore
- (void) load
{
    if (self.request != nil) return;
    self.request = [[SKProductsRequest alloc] initWithProductIdentifiers: [NSSet setWithObject: hhsFullProduct]];
    self.request.delegate = self;
    [self.request start];
}
- (void) buy
{
    if ([SKPaymentQueue canMakePayments] == NO) { hhsTell ("Purchases are turned off on this device (Screen Time > Content & Privacy)."); return; }
    if (self.product == nil) { self.buyWhenLoaded = YES; [self load]; return; }
    [[SKPaymentQueue defaultQueue] addPayment: [SKPayment paymentWithProduct: self.product]];
}
- (void) productsRequest: (SKProductsRequest*) r didReceiveResponse: (SKProductsResponse*) response
{
    SKProduct* p = response.products.firstObject;
    dispatch_async (dispatch_get_main_queue(), ^{
        self.request = nil;
        if (p != nil) self.product = p;
        if (self.buyWhenLoaded) { self.buyWhenLoaded = NO; if (p != nil) [self buy]; else hhsTell ("The App Store didn't answer. Check your connection and try again."); }
        else hhsTell ({});
    });
}
- (void) request: (SKRequest*) r didFailWithError: (NSError*) error
{
    dispatch_async (dispatch_get_main_queue(), ^{
        self.request = nil;
        if (self.buyWhenLoaded) { self.buyWhenLoaded = NO; hhsTell ("The App Store didn't answer. Check your connection and try again."); }
    });
}
- (void) paymentQueue: (SKPaymentQueue*) queue updatedTransactions: (NSArray<SKPaymentTransaction*>*) transactions
{
    for (SKPaymentTransaction* t in transactions)
    {
        const bool ours = [t.payment.productIdentifier isEqualToString: hhsFullProduct];
        switch (t.transactionState)
        {
            case SKPaymentTransactionStatePurchased:
            case SKPaymentTransactionStateRestored:
            {
                if (ours)
                {
                    NSDate* when = t.originalTransaction != nil && t.transactionState == SKPaymentTransactionStateRestored ? t.originalTransaction.transactionDate : t.transactionDate;
                    const double until = (when != nil ? [when timeIntervalSince1970] : [[NSDate date] timeIntervalSince1970]) + 33.0 * 86400.0;
                    auto* d = [NSUserDefaults standardUserDefaults];
                    if (until > [d doubleForKey: hhsUntilKey]) [d setDouble: until forKey: hhsUntilKey];
                }
                [queue finishTransaction: t];
                if (ours && t.transactionState == SKPaymentTransactionStatePurchased) hhsTell (hhsFullNow() ? "HumHouse Pro is unlocked. Thank you!" : std::string());
                break;
            }
            case SKPaymentTransactionStateFailed:
            {
                const bool cancelled = t.error != nil && t.error.code == SKErrorPaymentCancelled;
                std::string msg = cancelled || t.error == nil ? std::string() : std::string ([t.error.localizedDescription UTF8String]);
                [queue finishTransaction: t];
                if (ours) hhsTell (msg);
                break;
            }
            case SKPaymentTransactionStateDeferred:
                if (ours) hhsTell ("Waiting for approval (Ask to Buy). HumHouse Pro unlocks as soon as it's approved.");
                break;
            default: break;
        }
    }
}
- (void) paymentQueueRestoreCompletedTransactionsFinished: (SKPaymentQueue*) queue
{
    hhsTell (hhsFullNow() ? "HumHouse Pro is restored." : "No active HumHouse Pro subscription on this Apple ID.");
}
- (void) paymentQueue: (SKPaymentQueue*) queue restoreCompletedTransactionsFailedWithError: (NSError*) error
{
    if (error.code != SKErrorPaymentCancelled) hhsTell ([error.localizedDescription UTF8String]);
}
@end

static HHSStore* hhsStore = nil;

bool hhsStoreStart (std::function<void (bool unlocked, std::string message)> onChange)
{
    hhsStoreChanged = std::move (onChange);
    if (hhsStore == nil)
    {
        hhsStore = [[HHSStore alloc] init];
        [[SKPaymentQueue defaultQueue] addTransactionObserver: hhsStore];
        [hhsStore load];
    }
    return hhsFullNow();
}
bool hhsStoreUnlocked() { return hhsFullNow(); }
void hhsStoreBuy() { if (hhsStore != nil) [hhsStore buy]; }
void hhsStoreRestore() { [[SKPaymentQueue defaultQueue] restoreCompletedTransactions]; }
void hhsStoreManage() { [[UIApplication sharedApplication] openURL: [NSURL URLWithString: @"https://apps.apple.com/account/subscriptions"] options: @{} completionHandler: nil]; }
std::string hhsStorePrice()   // the price in the buyer's own currency once the App Store answers
{
    SKProduct* p = hhsStore != nil ? hhsStore.product : nil;
    if (p == nil) return "$7.99";
    NSNumberFormatter* f = [[NSNumberFormatter alloc] init];
    f.numberStyle = NSNumberFormatterCurrencyStyle;
    f.locale = p.priceLocale;
    NSString* s = [f stringFromNumber: p.price];
    return s != nil ? std::string ([s UTF8String]) : "$7.99";
}
