#include "Plugins.h"

namespace isfx
{
static juce::File isfxDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("InSequence FX");
    d.createDirectory(); return d;
}

PluginRack::PluginRack()
{
    formats.addDefaultFormats();
    if (auto xml = juce::parseXML (listFile())) list.recreateFromXml (*xml);
}

juce::File PluginRack::listFile() const { return isfxDataDir().getChildFile ("plugins.xml"); }

juce::StringArray PluginRack::scan (std::function<void (float, const juce::String&)> progress)
{
    juce::StringArray failed;
    auto pedal = isfxDataDir().getChildFile ("plugin-scan-in-progress.txt");
    for (auto* f : formats.getFormats())
    {
        if (! f->canScanForPlugins()) continue;
        juce::PluginDirectoryScanner sc (list, *f, f->getDefaultLocationsToSearch(), true, pedal, false);
        juce::String next;
        while (true)
        {
            next = sc.getNextPluginFileThatWillBeScanned();
            if (progress) progress (sc.getProgress(), f->getName() + ": " + juce::File::createFileWithoutCheckingPath (next).getFileName());
            if (! sc.scanNextFile (true, next)) break;
        }
        failed.addArray (sc.getFailedFiles());
    }
    if (auto xml = list.createXml()) xml->writeTo (listFile());
    return failed;
}

bool PluginRack::add (const juce::PluginDescription& d, juce::String& error)
{
    auto p = formats.createPluginInstance (d, rate, block, error);
    if (p == nullptr) return false;
    p->enableAllBuses();
    p->prepareToPlay (rate, block);
    const juce::ScopedLock sl (lock);
    slots.push_back ({ std::move (p), false });
    return true;
}

void PluginRack::remove (int i) { std::unique_ptr<juce::AudioPluginInstance> dead; { const juce::ScopedLock sl (lock); if (i < 0 || i >= (int) slots.size()) return; dead = std::move (slots[(size_t) i].p); slots.erase (slots.begin() + i); } }
void PluginRack::setBypass (int i, bool b) { const juce::ScopedLock sl (lock); if (i >= 0 && i < (int) slots.size()) slots[(size_t) i].bypass = b; }
juce::String PluginRack::name (int i) const { const juce::ScopedLock sl (lock); return i >= 0 && i < (int) slots.size() ? slots[(size_t) i].p->getName() : juce::String(); }
bool PluginRack::bypassed (int i) const { const juce::ScopedLock sl (lock); return i >= 0 && i < (int) slots.size() && slots[(size_t) i].bypass; }
juce::AudioPluginInstance* PluginRack::instance (int i) { const juce::ScopedLock sl (lock); return i >= 0 && i < (int) slots.size() ? slots[(size_t) i].p.get() : nullptr; }

void PluginRack::prepare (double r, int b)
{
    const juce::ScopedLock sl (lock);
    rate = r; block = b;
    for (auto& s : slots) { s.p->releaseResources(); s.p->setRateAndBufferSizeDetails (r, b); s.p->prepareToPlay (r, b); }
}

void PluginRack::reset() { const juce::ScopedLock sl (lock); for (auto& s : slots) s.p->reset(); }

void PluginRack::process (float* L, float* R, int n)
{
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked() || slots.empty()) return;
    for (int off = 0; off < n; off += block)
    {
        int m = std::min (block, n - off);
        for (auto& s : slots)
        {
            if (s.bypass) continue;
            int ch = std::max ({ 2, s.p->getTotalNumInputChannels(), s.p->getTotalNumOutputChannels() });
            buf.setSize (ch, m, false, false, true); buf.clear();
            buf.copyFrom (0, 0, L + off, m); buf.copyFrom (1, 0, R + off, m);
            midi.clear();
            s.p->processBlock (buf, midi);
            juce::FloatVectorOperations::copy (L + off, buf.getReadPointer (0), m);
            juce::FloatVectorOperations::copy (R + off, buf.getReadPointer (s.p->getTotalNumOutputChannels() > 1 ? 1 : 0), m);
        }
    }
}

juce::var PluginRack::toVar() const
{
    const juce::ScopedLock sl (lock);
    juce::Array<juce::var> arr;
    for (auto& s : slots)
    {
        auto* o = new juce::DynamicObject();
        auto xml = s.p->getPluginDescription().createXml();
        o->setProperty ("desc", xml ? xml->toString() : juce::String());
        juce::MemoryBlock mb; s.p->getStateInformation (mb);
        o->setProperty ("state", mb.toBase64Encoding());
        o->setProperty ("bypass", s.bypass);
        arr.add (juce::var (o));
    }
    return arr;
}

void PluginRack::fromVar (const juce::var& v, juce::StringArray& errors)
{
    { std::vector<Slot> old; const juce::ScopedLock sl (lock); old.swap (slots); }
    if (! v.isArray()) return;
    for (auto& e : *v.getArray())
    {
        juce::PluginDescription d;
        auto xml = juce::parseXML (e["desc"].toString());
        if (xml == nullptr || ! d.loadFromXml (*xml)) continue;
        juce::String err;
        if (! add (d, err)) { errors.add (d.name + ": " + err); continue; }
        juce::MemoryBlock mb; mb.fromBase64Encoding (e["state"].toString());
        const juce::ScopedLock sl (lock);
        slots.back().p->setStateInformation (mb.getData(), (int) mb.getSize());
        slots.back().bypass = (bool) e["bypass"];
    }
}

PluginRack& TrackRacks::get (const juce::String& id)
{
    const juce::ScopedLock sl (lock);
    auto& r = racks[id];
    if (r == nullptr) { r = std::make_unique<PluginRack>(); r->prepare (kMixRate, 512); }
    return *r;
}
bool TrackRacks::has (const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    auto it = racks.find (id);
    return it != racks.end() && it->second->size() > 0;
}
void TrackRacks::process (const juce::String& id, float* L, float* R, int n)
{
    const juce::ScopedLock sl (lock);
    auto it = racks.find (id);
    if (it != racks.end()) it->second->process (L, R, n);
}
void TrackRacks::reset() { const juce::ScopedLock sl (lock); for (auto& r : racks) r.second->reset(); }
void TrackRacks::clear() { std::map<juce::String, std::unique_ptr<PluginRack>> old; { const juce::ScopedLock sl (lock); old.swap (racks); } }
void TrackRacks::load (const Project& p, juce::StringArray& errors)
{
    clear();
    for (auto& s : p.sequences)
        for (auto& t : s.tracks)
            if (t.id.isNotEmpty() && t.plugins.isArray() && t.plugins.size() > 0)
            {
                auto r = std::make_unique<PluginRack>();
                r->prepare (kMixRate, 512); r->fromVar (t.plugins, errors); r->prepare (kMixRate, 512);
                const juce::ScopedLock sl (lock); racks[t.id] = std::move (r);
            }
}
void TrackRacks::store (Project& p) const
{
    const juce::ScopedLock sl (lock);
    for (auto& s : p.sequences)
        for (auto& t : s.tracks)
            if (t.id.isNotEmpty())
            {
                auto it = racks.find (t.id);
                t.plugins = it != racks.end() ? it->second->toVar() : juce::var();
            }
}
}
