#include "License.h"
#include <juce_cryptography/juce_cryptography.h>
#include "KeyprintData.h"

namespace hhs::license
{
static const char* const publicModulus = "b44ff9a72f112b528144e5581831ad83333fb460deccb5f964a27a6f677c0f9c4e6f896d5c6650e5d125ddd6d44981d1c177a778a1460a3863bcca21f1008dd619277a4e60e8088f12a1ffa65a8577b81d5c0a6c61ab4c943c33393a6970ce505e15c6d59101bbf5a8502a58c48ec4a988e8bfeb7a6ce9ed23d63cce4ea15ebea20c2e0e0dddaec46a8c9f169a8f3d9a0655aad61bf0a27cf87a19d559a35a44a54947401153c8b2cb5c9160fc53dd2a466e98a1727fae47e2ba78b25be732f4ccd0dfae7eba301d4c5efe0c9e2ae92b2844fde4338f10c350c0cdf9138ec9f1c08424308fad8fe71ab9a7e5046589151d7ae1aa156951dbfad6015a8a65021d";

static const char* const passModulus = "ba6a39405f87063595735269dc95cd1dd84bebce9561d07260b5b1e912c47d64c52f2b7e6a38e3f230b78faa0daf0985c9a1686181a990933143f2280d402c0445d518a3118983b7b571b4b895ddd849ed82adf1b2cc7953b5ebd9c6ee29566a15bd9dfbac4528174f0ad45d443e1848ee265b64bda4fb55a763db9714e4a4f227cc6e7029ef26362ad9966fbed9addf615fa42785086345948023c2adf53a6cefc1adf64749adf6eea606ce772f22fec8e50e5dd8c40412a4f2e7f217a279906f45067870ff9e5bff3a483d9cd3d204e70e0da794abe00aeeef9eb76bf477cdd95b6dff60e1d0ecbc7a483e901ae10f7228a308292f1baeeb8009996d37cc49";

#ifndef HHS_LICENSE_URL
 #define HHS_LICENSE_URL "https://humhouse-license.humhouse.workers.dev"
#endif

juce::String machineId()
{
    auto raw = juce::SystemStats::getUniqueDeviceID();
    if (raw.isEmpty()) raw = juce::SystemStats::getComputerName() + "|" + juce::SystemStats::getLogonName();
    const auto h = juce::SHA256 (("HumHouse|" + raw).toUTF8()).toHexString().toUpperCase();
    return h.substring (0, 4) + "-" + h.substring (4, 8) + "-" + h.substring (8, 12) + "-" + h.substring (12, 16);
}

static juce::BigInteger fromBytes (const void* data, size_t size)
{
    juce::BigInteger b;
    b.parseString (juce::String::toHexString (data, (int) size, 0), 16);
    return b;
}

// RSA PKCS#1 v1.5 / SHA-256 signature check against a public modulus (e = 65537)
static bool rsaVerify (const juce::MemoryBlock& payload, const juce::String& sigHex, const char* modulus)
{
    if (sigHex.length() < 64 || ! sigHex.containsOnly ("0123456789abcdefABCDEF")) return false;
    juce::BigInteger n, s;
    n.parseString (modulus, 16);
    s.parseString (sigHex, 16);
    if (s.compareAbsolute (n) >= 0) return false;
    s.exponentModulo (juce::BigInteger (65537), n);
    static const unsigned char prefix[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    const int k = (n.getHighestBit() + 8) / 8, tLen = (int) sizeof (prefix) + 32;
    if (k < tLen + 11) return false;
    juce::HeapBlock<unsigned char> em ((size_t) k, true);
    em[1] = 1;
    for (int i = 2; i < k - tLen - 1; ++i) em[i] = 0xff;
    std::memcpy (em + (k - tLen), prefix, sizeof (prefix));
    const auto digest = juce::SHA256 (payload.getData(), payload.getSize()).getRawData();
    std::memcpy (em + (k - 32), digest.getData(), 32);
    return s == fromBytes (em, (size_t) k);
}

Info verifyFor (const juce::String& keyIn, const juce::String& machine)
{
    Info info;
    info.error = "Invalid license key.";
    auto key = keyIn.removeCharacters (" \t\r\n");
    if (key.startsWith ("HHS1.")) key = key.substring (5);
    const int dot = key.indexOfChar ('.');
    if (dot <= 0) return info;
    juce::MemoryOutputStream payload;
    if (! juce::Base64::convertFromBase64 (payload, key.substring (0, dot)) || payload.getDataSize() == 0) return info;
    if (! rsaVerify (payload.getMemoryBlock(), key.substring (dot + 1), publicModulus)) return info;

    const auto v = juce::JSON::parse (payload.toString());
    if (v["product_sku"].toString() != "humhouse_studio") return info;
    info.name = v["name"].toString(); info.email = v["user_email"].toString(); info.licId = v["lic_id"].toString();
    const auto expires = (juce::int64) v["expires_at"];
    if (expires > 0 && juce::Time::currentTimeMillis() / 1000 > expires) { info.error = "This license key has expired."; return info; }
    if ((bool) v["any_machine"]) { info.ok = true; info.error = {}; return info; }   // retail key: works by itself, no Machine ID
    const auto* ids = v["hardware_ids"].getArray();
    if (ids == nullptr || ids->isEmpty() || ids->size() > 2) return info;
    for (auto& id : *ids)
        if (id.toString().trim().equalsIgnoreCase (machine.trim())) { info.ok = true; info.error = {}; return info; }
    info.error = "This key is locked to other computers (Machine ID " + machine + " is not on it).";
    return info;
}

juce::File keyFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("HumHouse").getChildFile ("license.key");
}

bool storedValid()
{
    const auto f = keyFile();
    return f.existsAsFile() && verify (f.loadFileAsString()).ok;
}
}

namespace hhs::license
{
Pass verifyPass (const juce::String& passIn, const juce::String& machine, juce::int64 now)
{
    Pass p;
    p.error = "Not activated.";
    auto s = passIn.removeCharacters (" \t\r\n");
    if (! s.startsWith ("HHP1.")) return p;
    s = s.substring (5);
    const int dot = s.indexOfChar ('.');
    if (dot <= 0) return p;
    juce::MemoryOutputStream payload;
    if (! juce::Base64::convertFromBase64 (payload, s.substring (0, dot)) || payload.getDataSize() == 0) return p;
    if (! rsaVerify (payload.getMemoryBlock(), s.substring (dot + 1), passModulus)) { p.error = "Activation is invalid."; return p; }
    const auto v = juce::JSON::parse (payload.toString());
    if (v["sku"].toString() != "humhouse_studio") return p;
    if (! v["mid"].toString().equalsIgnoreCase (machine.trim())) { p.error = "This activation belongs to another computer."; return p; }
    p.issued = (juce::int64) v["iat"]; p.expires = juce::jmin ((juce::int64) v["exp"], p.issued + 86400);   // daily check-in, older long passes too
    if (p.issued > now + 86400) { p.error = "Your computer's clock is wrong."; return p; }
    if (now >= p.expires) { p.expired = true; p.error = "Connect to the internet so HumHouse can check your key (once a day)."; return p; }
    p.ok = true; p.error = {};
    return p;
}

juce::File passFile() { return keyFile().getSiblingFile ("license.pass"); }

Pass storedPass()
{
    const auto f = passFile();
    if (! f.existsAsFile()) return {};
    const auto now = juce::Time::currentTimeMillis() / 1000;
    auto p = verifyPass (f.loadFileAsString(), machineId(), now);
    // clock turned back to stretch a pass: needs an online check
    const auto seenFile = f.getSiblingFile ("license.seen");
    const auto seen = seenFile.loadFileAsString().getLargeIntValue();
    if (p.ok && seen > 0 && now < seen - 3600)
    {
        p.ok = false; p.expired = true;
        p.error = "Your computer's clock went back: connect to the internet so HumHouse can check your key.";
        return p;
    }
    if (now > seen) seenFile.replaceWithText (juce::String (now));
    return p;
}

static juce::var postJson (const juce::String& url, const juce::var& body, int timeoutMs, int& status)
{
    auto in = juce::URL (url).withPOSTData (juce::JSON::toString (body, true))
                  .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                          .withExtraHeaders ("Content-Type: application/json")
                                          .withConnectionTimeoutMs (timeoutMs)
                                          .withStatusCode (&status));
    if (in == nullptr) return {};
    const auto v = juce::JSON::parse (in->readEntireStreamAsString());
    return v.isVoid() ? juce::var (juce::String()) : v;   // reached but unreadable: not void
}

juce::String homeServerUrl()
{
    auto in = juce::URL (serverUrl() + "/v1/servers")
                  .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs (5000));
    if (in == nullptr) return {};
    const auto u = juce::JSON::parse (in->readEntireStreamAsString())["home"].toString();
    return u.startsWith ("http://") || u.startsWith ("https://") ? u : juce::String();
}

// the key goes to the home server RSA-encrypted (PKCS#1 v1.5) to the pass key, so only that server can read it
juce::String encryptForServer (const juce::String& text)
{
    juce::BigInteger n; n.parseString (passModulus, 16);
    const int k = (n.getHighestBit() + 8) / 8;
    const auto* m = text.toRawUTF8();
    const int mLen = (int) std::strlen (m);
    if (mLen > k - 11) return {};
    juce::HeapBlock<unsigned char> em ((size_t) k, true);
    em[1] = 2;
    juce::Random rnd;
    for (int i = 2; i < k - mLen - 1; ++i) em[i] = (unsigned char) (1 + rnd.nextInt (255));
    std::memcpy (em + (k - mLen), m, (size_t) mLen);
    auto c = fromBytes (em, (size_t) k);
    c.exponentModulo (juce::BigInteger (65537), n);
    return c.toString (16, k * 2);
}

juce::String serverUrl() { return HHS_LICENSE_URL; }

// exactly what the license server does (normalizeKey in server/license/src/index.js)
static juce::String normKey (const juce::String& in)
{
    auto s = in.trim().toUpperCase();
    if (s.startsWith ("HHS")) s = s.substring (3);
    s = s.retainCharacters ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ").replaceCharacter ('O', '0').replaceCharacter ('I', '1').replaceCharacter ('L', '1');
    return s.length() == 20 && s.containsOnly ("0123456789ABCDEFGHJKMNPQRSTVWXYZ") ? s : juce::String();
}
static juce::String prettyKey (const juce::String& n)
{
    return "HHS-" + n.substring (0, 5) + "-" + n.substring (5, 10) + "-" + n.substring (10, 15) + "-" + n.substring (15, 20);
}
juce::String findKey (const juce::String& text)
{
    const auto up = text.toUpperCase();
    for (int i = up.indexOf ("HHS"); i >= 0; i = up.indexOf (i + 1, "HHS"))
    {
        juce::String raw;
        for (int j = i + 3; j < up.length() && raw.length() < 20; ++j)
        {
            const auto c = up[j];
            if (juce::CharacterFunctions::isLetterOrDigit (c)) raw += juce::String::charToString (c);
            else if (! juce::String (" -_.\t\r\n").containsChar (c)) break;
        }
        if (const auto n = normKey (raw); n.isNotEmpty()) return prettyKey (n);
    }
    const auto n = normKey (text);
    return n.isNotEmpty() ? prettyKey (n) : juce::String();
}
// 16-byte fingerprints: matching one by brute force would take ~2^110 tries
static constexpr size_t kPrint = 16;
bool knownKey (const juce::String& key)
{
    const auto n = normKey (key);
    if (n.isEmpty()) return false;
    const auto d = juce::SHA256 (("HHSOFF|" + n).toUTF8()).getRawData();
    const auto* p = reinterpret_cast<const unsigned char*> (KeyprintData::keyprints_bin);
    size_t lo = 0, hi = (size_t) KeyprintData::keyprints_binSize / kPrint;
    while (lo < hi)
    {
        const auto mid = (lo + hi) / 2;
        const int c = std::memcmp (p + mid * kPrint, d.getData(), kPrint);
        if (c == 0) return true;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}
static juce::File graceFile() { return keyFile().getSiblingFile ("license.grace"); }
static bool graceTimeOk()
{
    const auto t = graceFile().loadFileAsString().getLargeIntValue(), now = juce::Time::currentTimeMillis() / 1000;
    return t > 0 && now >= t - 3600 && now - t < 7 * 86400;
}
bool graceOk() { return knownKey (keyFile().loadFileAsString()) && graceTimeOk(); }

Online activateOnline (const juce::String& key, const juce::String& machine)
{
    Online r;
    auto* o = new juce::DynamicObject();
    o->setProperty ("key", key.trim());
    o->setProperty ("mid", machine);
    o->setProperty ("ver", JUCE_APPLICATION_VERSION_STRING);
    const juce::var body (o);
    int status = 0;
    juce::var v;
    // the owner's own server first; Cloudflare whenever it's off or doesn't answer properly
    if (const auto home = key.trim().isNotEmpty() ? homeServerUrl() : juce::String(); home.isNotEmpty())
    {
        auto* h = new juce::DynamicObject();
        h->setProperty ("ek", encryptForServer (key.trim()));
        h->setProperty ("mid", machine);
        h->setProperty ("ver", JUCE_APPLICATION_VERSION_STRING);
        v = postJson (home + "/v1/activate", juce::var (h), 6000, status);
        if (! v.isObject() || status >= 500) v = juce::var();
    }
    if (key.trim().isEmpty())   // no key saved/pasted: an already-activated computer unlocks from its Machine ID
        v = postJson (serverUrl() + "/v1/restore", body, 12000, status);
    else if (! v.isObject()) v = postJson (serverUrl() + "/v1/activate", body, 12000, status);
    // server off / busy / lost its data, and this is one of our sold keys: unlock now, confirm online later (7 days max)
    const bool down = v.isVoid() || ! v.isObject() || status == 0 || status >= 500 || (status == 404 && ! (bool) v["revoked"]);
    if (key.trim().isNotEmpty() && down && ! (bool) v["ok"] && knownKey (key))
    {
        if (! graceFile().existsAsFile()) { graceFile().getParentDirectory().createDirectory(); graceFile().replaceWithText (juce::String (juce::Time::currentTimeMillis() / 1000)); }
        if (graceTimeOk()) { r.ok = r.provisional = true; return r; }
        r.error = "Connect to the internet once so HumHouse can confirm your key."; return r;
    }
    if (v.isVoid()) { r.error = "Can't reach the HumHouse license server. Check your internet and try again."; return r; }
    if (! v.isObject()) { r.error = "The license server didn't answer properly. Try again in a minute."; return r; }
    if ((bool) v["ok"])
    {
        r.pass = v["pass"].toString();
        r.ok = verifyPass (r.pass, machine, juce::Time::currentTimeMillis() / 1000).ok;
        if (! r.ok) r.error = "The license server's answer couldn't be verified.";
        return r;
    }
    r.error = v["error"].toString().isNotEmpty() ? v["error"].toString() : juce::String ("Activation failed.");
    r.revoked = (bool) v["revoked"];
    r.lockNow = r.revoked || (key.trim().isNotEmpty() && (status == 403 || status == 404 || (bool) v["full"]));
    return r;
}

bool saveActivation (const juce::String& key, const juce::String& pass)
{
    keyFile().getParentDirectory().createDirectory();
    if (pass.isEmpty()) return keyFile().replaceWithText (key.trim());
    graceFile().deleteFile();
    return keyFile().replaceWithText (key.trim()) && passFile().replaceWithText (pass);
}

void clearActivation() { passFile().deleteFile(); }
}
