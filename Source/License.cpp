#include "License.h"
#include <juce_cryptography/juce_cryptography.h>

namespace hhs::license
{
static const char* const publicModulus = "b44ff9a72f112b528144e5581831ad83333fb460deccb5f964a27a6f677c0f9c4e6f896d5c6650e5d125ddd6d44981d1c177a778a1460a3863bcca21f1008dd619277a4e60e8088f12a1ffa65a8577b81d5c0a6c61ab4c943c33393a6970ce505e15c6d59101bbf5a8502a58c48ec4a988e8bfeb7a6ce9ed23d63cce4ea15ebea20c2e0e0dddaec46a8c9f169a8f3d9a0655aad61bf0a27cf87a19d559a35a44a54947401153c8b2cb5c9160fc53dd2a466e98a1727fae47e2ba78b25be732f4ccd0dfae7eba301d4c5efe0c9e2ae92b2844fde4338f10c350c0cdf9138ec9f1c08424308fad8fe71ab9a7e5046589151d7ae1aa156951dbfad6015a8a65021d";

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
    const auto sigHex = key.substring (dot + 1);
    if (sigHex.length() < 64 || ! sigHex.containsOnly ("0123456789abcdefABCDEF")) return info;

    juce::BigInteger n, s;
    n.parseString (publicModulus, 16);
    s.parseString (sigHex, 16);
    if (s.compareAbsolute (n) >= 0) return info;
    s.exponentModulo (juce::BigInteger (65537), n);

    // expected PKCS#1 v1.5 block: 00 01 FF..FF 00 DigestInfo(SHA-256) hash
    static const unsigned char prefix[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    const int k = (n.getHighestBit() + 8) / 8, tLen = (int) sizeof (prefix) + 32;
    if (k < tLen + 11) return info;
    juce::HeapBlock<unsigned char> em ((size_t) k, true);
    em[1] = 1;
    for (int i = 2; i < k - tLen - 1; ++i) em[i] = 0xff;
    std::memcpy (em + (k - tLen), prefix, sizeof (prefix));
    const auto digest = juce::SHA256 (payload.getData(), payload.getDataSize()).getRawData();
    std::memcpy (em + (k - 32), digest.getData(), 32);
    if (s != fromBytes (em, (size_t) k)) return info;

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
