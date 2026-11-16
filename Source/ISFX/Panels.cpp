#include "App.h"
#include "Editing.h"
#include <cstring>

namespace isfx
{
// ============================================================ Preview
Preview::Preview (AppState& s) : juce::Thread ("ISFX preview"), st (s)
{
    setWantsKeyboardFocus (true);
    snap = std::make_shared<Project> (st.project);
    startThread();
}

static juce::Image rgbaToImage (const std::vector<juce::uint8>& px, int W, int H)
{
    juce::Image im (juce::Image::ARGB, W, H, false);
    juce::Image::BitmapData bd (im, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < H; ++y)
    {
        auto* src = px.data() + (size_t) y * W * 4;
        auto* dst = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer (y));
        for (int x = 0; x < W; ++x, src += 4)
            dst[x].setARGB (255, src[0], src[1], src[2]);
    }
    return im;
}

static juce::int64 projectHash (const Project& p) { return juce::JSON::toString (toVar (p), true).hashCode64(); }

// whole-timeline render on its own low-priority thread with its own decoders; frames kept as JPEG in memory
struct Preview::RenderAll : juce::Thread
{
    RenderAll (std::shared_ptr<const Project> p, int div) : juce::Thread ("ISFX render all"), proj (std::move (p))
    {
        hash = projectHash (*proj);
        outputSize (*proj, std::max (90, 1080 / std::max (1, div)), W, H);
        fps = proj->fps > 0 ? proj->fps : 30.0;
        total = (int) std::max<juce::int64> (1, ticksToFrame (proj->seq().duration(), fps) + 1);
        frames.resize ((size_t) total);
        startThread (juce::Thread::Priority::low);
    }
    ~RenderAll() override { stopThread (10000); }
    void run() override
    {
        Renderer r; std::vector<juce::uint8> px; juce::JPEGImageFormat jpg; jpg.setQuality (0.9f);
        for (int f = 0; f < total && ! threadShouldExit(); ++f)
        {
            while (hostBusy().load() && ! threadShouldExit()) wait (50);
            px.clear();
            if (r.render (*proj, proj->seq(), frameToTicks (f, fps), W, H, px, H) && px.size() >= (size_t) W * H * 4)
            {
                juce::MemoryOutputStream mo;
                jpg.writeImageToStream (rgbaToImage (px, W, H), mo);
                frames[(size_t) f] = mo.getMemoryBlock();
            }
            done = f + 1;
        }
    }
    juce::Image frameAt (Ticks t) const
    {
        const auto f = ticksToFrame (t, fps);
        if (f < 0 || f >= done.load()) return {};
        auto& mb = frames[(size_t) f];
        return mb.getSize() > 0 ? juce::ImageFileFormat::loadFrom (mb.getData(), mb.getSize()) : juce::Image();
    }
    std::shared_ptr<const Project> proj;
    juce::int64 hash = 0;
    int W = 0, H = 0, total = 1;
    double fps = 30;
    std::vector<juce::MemoryBlock> frames;
    std::atomic<int> done { 0 };
};

void Preview::startRenderAll (int div)
{
    std::shared_ptr<const Project> p; { const juce::ScopedLock sl (lock); p = snap; }
    if (p == nullptr) return;
    auto n = std::make_shared<RenderAll> (p, div);
    std::shared_ptr<RenderAll> old;
    { const juce::ScopedLock sl (lock); old = std::move (rall); rall = n; }
}

void Preview::cancelRenderAll()
{
    std::shared_ptr<RenderAll> old;
    { const juce::ScopedLock sl (lock); old = std::move (rall); }
    if (old) old->signalThreadShouldExit();
}

float Preview::renderAllProgress() const
{
    std::shared_ptr<RenderAll> r; { const juce::ScopedLock sl (lock); r = rall; }
    return r ? (float) r->done.load() / (float) r->total : -1.0f;
}

void Preview::projectEdited (const Project& p)
{
    std::shared_ptr<RenderAll> r; { const juce::ScopedLock sl (lock); r = rall; }
    if (r && projectHash (p) != r->hash) cancelRenderAll();
}

Preview::~Preview() { if (baker) baker->stopThread (6000); signalThreadShouldExit(); wake.signal(); stopThread (4000); }

void Preview::setProject (const Project& p)
{
    auto n = std::make_shared<const Project> (p);
    const auto sig = juce::JSON::toString (toVar (p), true).hashCode64();
    if (sig != st.rcache.sig.load())
    {
        const juce::ScopedLock cl (st.rcache.lock);
        st.rcache.frames.clear(); st.rcache.bytes = 0; st.rcache.sig = sig;
    }
    const juce::ScopedLock sl (lock); snap = n;
}

static juce::Image toImageRGBA (const std::vector<juce::uint8>& px, int W, int H)
{
    juce::Image im (juce::Image::ARGB, W, H, false);
    juce::Image::BitmapData bd (im, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < H; ++y)
    {
        auto* src = px.data() + (size_t) y * W * 4;
        auto* dst = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer (y));
        for (int x = 0; x < W; ++x, src += 4) dst[x].setARGB (255, src[0], src[1], src[2]);
    }
    return im;
}

void Preview::renderRed()
{
    if (rendering()) return;
    baker = std::make_unique<Baker> (*this);
    baker->startThread (juce::Thread::Priority::low);
}

void Preview::bake()
{
    std::shared_ptr<const Project> p; int W, H;
    { const juce::ScopedLock sl (lock); p = snap; W = pausedW; H = pausedH; }
    if (p == nullptr) return;
    if (W <= 0 || H <= 0) outputSize (*p, 720, W, H);
    const auto sig = st.rcache.sig.load();
    {
        const juce::ScopedLock cl (st.rcache.lock);
        if (st.rcache.w != W || st.rcache.h != H) { st.rcache.frames.clear(); st.rcache.bytes = 0; st.rcache.w = W; st.rcache.h = H; }
    }
    auto& s = p->seq();
    const double fr = p->fps;
    std::vector<juce::int64> todo;
    for (auto& c : s.clips)
        if (needsRender (*p, s, c))
        {
            const auto f0 = (juce::int64) std::ceil (ticksToSec (c.start) * fr - 1e-6), f1 = (juce::int64) std::ceil (ticksToSec (c.end()) * fr - 1e-6);
            for (auto f = f0; f < f1; ++f) todo.push_back (f);
        }
    std::sort (todo.begin(), todo.end());
    todo.erase (std::unique (todo.begin(), todo.end()), todo.end());
    juce::Component::SafePointer<Preview> safe (this);
    auto ping = [safe] { juce::MessageManager::callAsync ([safe] { if (safe && safe->onCacheChanged) safe->onCacheChanged(); }); };
    std::vector<juce::uint8> px;
    juce::JPEGImageFormat jpg; jpg.setQuality (0.92f);
    bakeRenderer.setLive (false);
    int done = 0;
    for (auto f : todo)
    {
        if (juce::Thread::currentThreadShouldExit() || st.rcache.sig.load() != sig) break;
        { const juce::ScopedLock cl (st.rcache.lock); if (st.rcache.frames.count (f) || st.rcache.bytes > ((size_t) 2 << 30)) continue; }
        if (! bakeRenderer.render (*p, s, frameToTicks (f, fr), W, H, px, 1080)) continue;
        juce::MemoryOutputStream mo;
        if (! jpg.writeImageToStream (toImageRGBA (px, W, H), mo)) continue;
        {
            const juce::ScopedLock cl (st.rcache.lock);
            if (st.rcache.sig.load() != sig) break;
            st.rcache.bytes += mo.getDataSize();
            st.rcache.frames[f] = mo.getMemoryBlock();
        }
        if (++done % 10 == 0) ping();
    }
    bakeRenderer.clearSources();
    ping();
}

juce::Rectangle<int> Preview::frameArea() const
{
    auto r = getLocalBounds().reduced (8).withTrimmedBottom (18);
    int W, H; outputSize (st.project, 1080, W, H);
    float a = (float) W / H;
    int w = r.getWidth(), h = (int) (w / a);
    if (h > r.getHeight()) { h = r.getHeight(); w = (int) (h * a); }
    return juce::Rectangle<int> (w, h).withCentre (r.getCentre());
}

void Preview::requestFrame (Ticks t)
{
    auto fa = frameArea();
    // preview renders at most ~540p (export renders at full size through the same renderer)
    double scale = 1.0;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds())) scale = d->scale;
    int phys = (int) std::round (std::min (fa.getWidth(), fa.getHeight()) * scale);
    // playback renders a light preview from proxy-sized decodes; a paused frame renders sharp at screen resolution
    // Fixed decode tiers (1080/div) so resizing the window never opens extra decoders; 1/2 and below read the proxy.
    const int tier = live ? std::max (90, 1080 / std::max (1, quality.load())) : 1080;
    int sh = juce::jlimit (90, tier, phys);
    if (live)
    {   // effect-heavy frames over budget: shrink the live render size (decode tier unchanged); paused frames stay full
        const double ms = avgMs.load();
        if (ms > 45) sh = std::min (sh, 540); else if (ms > 30) sh = std::min (sh, 720);
    }
    int W, H; outputSize (st.project, sh, W, H);
    const juce::ScopedLock sl (lock);
    want = t; lastReq = t; wantW = W; wantH = H; wantDecodeH = tier;
    if (! live) { pausedW = W; pausedH = H; }
    wake.signal();
}

void Preview::run()
{
    std::vector<juce::uint8> px;
    while (! threadShouldExit())
    {
        wake.wait (100);
        Ticks t; int W, H, decH; std::shared_ptr<const Project> p;
        {
            const juce::ScopedLock sl (lock);
            if (want < 0) continue;
            t = want; want = -1; W = wantW; H = wantH; p = snap; decH = wantDecodeH;
        }
        if (W <= 0 || H <= 0 || p == nullptr) continue;
        if (live.load())
        {
            std::shared_ptr<RenderAll> ra; { const juce::ScopedLock sl (lock); ra = rall; }
            if (ra)
                if (auto im = ra->frameAt (t); im.isValid())
                {
                    { const juce::ScopedLock sl (lock); shown = im; }
                    triggerAsyncUpdate();
                    continue;
                }
        }
        if (clearReq.exchange (false)) renderer.clearSources();
        if (live.load())
        {   // rendered (green) section: show the full-quality cached frame, no render at all
            const auto f = (juce::int64) std::floor (ticksToSec (t) * p->fps + 1e-6);
            juce::MemoryBlock mb;
            {
                const juce::ScopedLock cl (st.rcache.lock);
                if (auto it = st.rcache.frames.find (f); it != st.rcache.frames.end()) mb = it->second;
            }
            if (mb.getSize() > 0)
                if (auto im = juce::ImageFileFormat::loadFrom (mb.getData(), mb.getSize()); im.isValid())
                {
                    { const juce::ScopedLock sl (lock); shown = im; }
                    triggerAsyncUpdate();
                    continue;
                }
        }
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        renderer.setLive (live.load());
        renderer.render (*p, p->seq(), t, W, H, px, decH);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        lastMs = ms;
        avgMs = avgMs.load() <= 0 ? ms : avgMs.load() * 0.85 + ms * 0.15;   // steady readout, no flicker
        auto im = rgbaToImage (px, W, H);
        { const juce::ScopedLock sl (lock); shown = im; }
        triggerAsyncUpdate();
    }
}

static bool showsVideo (const Project& p, const Sequence& s, const Clip& c, Ticks t)
{
    if (! c.enabled || (c.mediaId.isEmpty() && c.text.isEmpty()) || t < c.start || t >= c.end()) return false;
    if (c.track < 0 || c.track >= (int) s.tracks.size()) return false;
    auto& tr = s.tracks[(size_t) c.track];
    if (tr.kind != TrackKind::Video || tr.hidden) return false;
    if (c.text.isNotEmpty()) return true;
    auto* m = p.findMedia (c.mediaId);
    return m != nullptr && m->kind != MediaKind::Audio;
}

const Clip* Preview::shownClip (const juce::String& id) const
{
    auto& s = st.project.seq();
    for (auto& c : s.clips) if (c.id == id) return showsVideo (st.project, s, c, st.playhead) ? &c : nullptr;
    return nullptr;
}

bool Preview::geomFor (const Clip& c, ClipGeom& g) const
{
    auto fa = frameArea().toFloat();
    if (! clipGeometry (st.project, c, fa.getWidth(), fa.getHeight(), g)) return false;
    g.cx += fa.getX(); g.cy += fa.getY();
    return true;
}

Preview::Handle Preview::hitHandle (const ClipGeom& g, juce::Point<float> p, int& idx) const
{
    const float mx = (g.x0 + g.x1) * 0.5f, my = (g.y0 + g.y1) * 0.5f;
    auto top = g.toScreen (mx, g.y0);
    juce::Point<float> knob { top.x - g.sr * 26, top.y - g.cr * 26 };
    if (p.getDistanceFrom (knob) <= 9) return Handle::Rotate;
    const juce::Point<float> corners[] = { g.toScreen (g.x0, g.y0), g.toScreen (g.x1, g.y0), g.toScreen (g.x1, g.y1), g.toScreen (g.x0, g.y1) };
    for (idx = 0; idx < 4; ++idx) if (p.getDistanceFrom (corners[idx]) <= 8) return Handle::Corner;
    const juce::Point<float> edges[] = { g.toScreen (g.x0, my), g.toScreen (g.x1, my), g.toScreen (mx, g.y0), g.toScreen (mx, g.y1) };
    for (idx = 0; idx < 4; ++idx) if (p.getDistanceFrom (edges[idx]) <= 8) return Handle::Edge;
    idx = 0;
    return g.contains (p) ? Handle::Move : Handle::None;
}

void Preview::mouseMove (const juce::MouseEvent& e)
{
    juce::MouseCursor cur = juce::MouseCursor::NormalCursor;
    if (! live && st.selected.size() == 1)
        if (auto* c = shownClip (st.selected[0]))
        {
            ClipGeom g; int idx = 0;
            if (geomFor (*c, g))
                switch (hitHandle (g, e.position, idx))
                {
                    case Handle::Move: cur = juce::MouseCursor::DraggingHandCursor; break;
                    case Handle::Corner: cur = juce::MouseCursor::BottomRightCornerResizeCursor; break;
                    case Handle::Edge: cur = idx < 2 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor; break;
                    case Handle::Rotate: cur = juce::MouseCursor::CrosshairCursor; break;
                    case Handle::None: break;
                }
        }
    setMouseCursor (cur);
}

void Preview::mouseDown (const juce::MouseEvent& e)
{
    handle = Handle::None; movedHandle = false;
    if (live || e.mods.isPopupMenu()) return;
    const Clip* c = st.selected.size() == 1 ? shownClip (st.selected[0]) : nullptr;
    ClipGeom g;
    if (c != nullptr && geomFor (*c, g)) handle = hitHandle (g, e.position, handleIdx);
    if (handle == Handle::None)
    {
        // pick the top-most visible clip under the mouse (higher track = on top)
        c = nullptr;
        auto& s = st.project.seq();
        for (auto& o : s.clips)
            if (showsVideo (st.project, s, o, st.playhead) && (c == nullptr || o.track > c->track) && geomFor (o, g) && g.contains (e.position))
                c = &o;
        if (c == nullptr) { if (! st.selected.isEmpty()) { st.selected.clear(); if (onSelect) onSelect(); } repaint(); return; }
        st.selected = { c->id };
        if (onSelect) onSelect();
        c = shownClip (st.selected[0]);
        if (c == nullptr) return;
        handle = c->locked ? Handle::None : Handle::Move;
    }
    if (c->locked) handle = Handle::None;
    if (handle == Handle::None) return;
    editId = c->id; before = st.project; fx0 = c->fx; geomFor (*c, g0); down = e.position;
}

void Preview::mouseDrag (const juce::MouseEvent& e)
{
    if (handle == Handle::None) return;
    Clip* c = st.project.seq().findClip (editId);
    if (c == nullptr) return;
    const auto& CP = clipParams();
    auto v0 = [&] (const char* id) { return fxValue (fx0, CP, id); };
    auto fa = frameArea().toFloat();
    auto p = e.position;
    c->fx = fx0;
    switch (handle)
    {
        case Handle::Move:
        {
            float dx = p.x - down.x, dy = p.y - down.y;
            if (e.mods.isShiftDown()) { if (std::abs (dx) > std::abs (dy)) dy = 0; else dx = 0; }
            c->fx.set ("posX", juce::jlimit (-2.0f, 2.0f, v0 ("posX") + dx / (fa.getWidth() * 0.5f)));
            c->fx.set ("posY", juce::jlimit (-2.0f, 2.0f, v0 ("posY") + dy / (fa.getHeight() * 0.5f)));
            break;
        }
        case Handle::Corner:
        {
            auto l0 = g0.toLocal (down), l = g0.toLocal (p);
            if (e.mods.isShiftDown())
            {
                c->fx.set ("scaleX", juce::jlimit (0.01f, 10.0f, v0 ("scaleX") * std::abs (l.x) / std::max (1.0f, std::abs (l0.x))));
                c->fx.set ("scaleY", juce::jlimit (0.01f, 10.0f, v0 ("scaleY") * std::abs (l.y) / std::max (1.0f, std::abs (l0.y))));
            }
            else
            {
                float f = l.getDistanceFromOrigin() / std::max (1.0f, l0.getDistanceFromOrigin());
                c->fx.set ("scale", juce::jlimit (0.01f, 10.0f, v0 ("scale") * f));
            }
            break;
        }
        case Handle::Edge:
        {
            auto l = g0.toLocal (p);
            const bool mir = v0 ("mirror") > 0.5f;
            const char* left = mir ? "cropR" : "cropL";
            const char* right = mir ? "cropL" : "cropR";
            const float hx = std::max (1.0f, g0.hx), hy = std::max (1.0f, g0.hy);
            if (handleIdx == 0) c->fx.set (left, juce::jlimit (0.0f, 0.98f - v0 (right), (l.x + hx) / (2 * hx)));
            if (handleIdx == 1) c->fx.set (right, juce::jlimit (0.0f, 0.98f - v0 (left), (hx - l.x) / (2 * hx)));
            if (handleIdx == 2) c->fx.set ("cropT", juce::jlimit (0.0f, 0.98f - v0 ("cropB"), (l.y + hy) / (2 * hy)));
            if (handleIdx == 3) c->fx.set ("cropB", juce::jlimit (0.0f, 0.98f - v0 ("cropT"), (hy - l.y) / (2 * hy)));
            break;
        }
        case Handle::Rotate:
        {
            float a0 = std::atan2 (down.y - g0.cy, down.x - g0.cx), a = std::atan2 (p.y - g0.cy, p.x - g0.cx);
            float deg = v0 ("rotation") + (a - a0) * 180.0f / juce::MathConstants<float>::pi;
            while (deg > 180) deg -= 360;
            while (deg < -180) deg += 360;
            if (e.mods.isShiftDown()) deg = std::round (deg / 15.0f) * 15.0f;
            c->fx.set ("rotation", deg);
            break;
        }
        case Handle::None: break;
    }
    movedHandle = true;
    if (onEdited) onEdited();
}

void Preview::mouseUp (const juce::MouseEvent&)
{
    if (handle != Handle::None && movedHandle) { st.history.push (before, "Transform"); st.dirty = true; }
    handle = Handle::None; movedHandle = false;
}

bool Preview::keyPressed (const juce::KeyPress& k)
{
    const int kc = k.getKeyCode();
    if (kc != juce::KeyPress::leftKey && kc != juce::KeyPress::rightKey && kc != juce::KeyPress::upKey && kc != juce::KeyPress::downKey) return false;
    if (live || st.selected.size() != 1) return false;
    auto* shown = shownClip (st.selected[0]);
    if (shown == nullptr || shown->locked) return false;
    auto fa = frameArea().toFloat();
    float step = k.getModifiers().isShiftDown() ? 10.0f : 1.0f;
    auto prev = st.project;
    Clip* c = st.project.seq().findClip (shown->id);
    const auto& CP = clipParams();
    if (kc == juce::KeyPress::leftKey || kc == juce::KeyPress::rightKey)
        c->fx.set ("posX", juce::jlimit (-1.0f, 1.0f, fxValue (c->fx, CP, "posX") + (kc == juce::KeyPress::leftKey ? -step : step) / (fa.getWidth() * 0.5f)));
    else
        c->fx.set ("posY", juce::jlimit (-1.0f, 1.0f, fxValue (c->fx, CP, "posY") + (kc == juce::KeyPress::upKey ? -step : step) / (fa.getHeight() * 0.5f)));
    st.history.push (prev, "Nudge"); st.dirty = true;
    if (onEdited) onEdited();
    return true;
}

void Preview::handleAsyncUpdate()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! live) { fps = 0; fpsCount = 0; fpsT0 = 0; }
    else if (fpsT0 <= 0) { fpsT0 = now; fpsCount = 0; }
    else if (++fpsCount, now - fpsT0 >= 500) { fps = fpsCount * 1000.0 / (now - fpsT0); fpsCount = 0; fpsT0 = now; }
    repaint();
}

void Preview::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff07080a));
    auto fa = frameArea();
    juce::Image im; { const juce::ScopedLock sl (lock); im = shown; }
    g.setColour (juce::Colours::black); g.fillRect (fa);
    if (im.isValid())
    {
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (im, fa.toFloat(), juce::RectanglePlacement::stretchToFit);
    }
    g.setColour (theme::line); g.drawRect (fa.expanded (1));
    if (! live && st.selected.size() == 1)
        if (auto* c = shownClip (st.selected[0]))
        {
            ClipGeom cg;
            if (geomFor (*c, cg))
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (getLocalBounds());
                const float mx = (cg.x0 + cg.x1) * 0.5f, my = (cg.y0 + cg.y1) * 0.5f;
                juce::Path box;
                box.startNewSubPath (cg.toScreen (cg.x0, cg.y0)); box.lineTo (cg.toScreen (cg.x1, cg.y0));
                box.lineTo (cg.toScreen (cg.x1, cg.y1)); box.lineTo (cg.toScreen (cg.x0, cg.y1)); box.closeSubPath();
                g.setColour (juce::Colours::black.withAlpha (0.5f)); g.strokePath (box, juce::PathStrokeType (3.0f));
                g.setColour (theme::accent); g.strokePath (box, juce::PathStrokeType (1.5f));
                auto top = cg.toScreen (mx, cg.y0);
                juce::Point<float> knob { top.x - cg.sr * 26, top.y - cg.cr * 26 };
                g.drawLine ({ top, knob }, 1.5f);
                g.setColour (juce::Colours::white); g.fillEllipse (juce::Rectangle<float> (11, 11).withCentre (knob));
                g.setColour (theme::accent); g.drawEllipse (juce::Rectangle<float> (11, 11).withCentre (knob), 1.5f);
                auto sq = [&] (juce::Point<float> q, bool round)
                {
                    auto r = juce::Rectangle<float> (9, 9).withCentre (q);
                    g.setColour (juce::Colours::white); round ? g.fillEllipse (r) : g.fillRect (r);
                    g.setColour (theme::accent); round ? g.drawEllipse (r, 1.2f) : g.drawRect (r, 1.2f);
                };
                for (auto q : { cg.toScreen (cg.x0, cg.y0), cg.toScreen (cg.x1, cg.y0), cg.toScreen (cg.x1, cg.y1), cg.toScreen (cg.x0, cg.y1) }) sq (q, false);
                for (auto q : { cg.toScreen (cg.x0, my), cg.toScreen (cg.x1, my), cg.toScreen (mx, cg.y0), cg.toScreen (mx, cg.y1) }) sq (q, true);
                auto ctr = cg.toScreen (mx, my);
                g.setColour (theme::accent2.withAlpha (0.8f));
                g.drawLine (ctr.x - 6, ctr.y, ctr.x + 6, ctr.y, 1.0f); g.drawLine (ctr.x, ctr.y - 6, ctr.x, ctr.y + 6, 1.0f);
            }
        }
    g.setColour (theme::dim); g.setFont (11.0f);
    double sec = ticksToSec (st.playhead);
    auto fr = ticksToFrame (st.playhead, st.project.fps) % std::max (1, (int) std::lround (st.project.fps));
    juce::String tc = juce::String ((int) sec / 60).paddedLeft ('0', 2) + ":" + juce::String ((int) sec % 60).paddedLeft ('0', 2) + ":" + juce::String ((int) fr).paddedLeft ('0', 2);
    juce::String info = tc;
    if (st.project.song.valid) info << "   bar " << (st.project.song.barAt (st.playhead) + 1) << "   " << juce::String (st.project.song.bpm, 1) << " BPM";
    info << "   " << st.project.aspectW << ":" << st.project.aspectH << "   " << st.project.seq().name;
    g.drawText (info, getLocalBounds().removeFromBottom (20).reduced (10, 0), juce::Justification::centredLeft);
    {
        const int q = quality.load();
        // juce::String (double, 0) prints full precision, hence integer rounding
        juce::String info = live ? juce::String (juce::roundToInt (fps)) + " fps   " + juce::String (juce::roundToInt (avgMs.load())) + " ms   " + (q <= 1 ? juce::String ("Full") : "1/" + juce::String (q))
                                 : juce::String (juce::roundToInt (lastMs.load())) + " ms";
        g.drawText (info, getLocalBounds().removeFromBottom (20).reduced (10, 0), juce::Justification::centredRight);
    }
    if (st.project.seq().clips.empty())
    {
        g.setColour (theme::dim); g.setFont (15.0f);
        g.drawText ("Import a song and your video takes, then press AUTO SYNC and GENERATE 3 EDITS", fa.reduced (20), juce::Justification::centred, true);
    }
}

// ============================================================ Media panel
MediaPanel::MediaPanel (AppState& s) : st (s)
{
    list.setModel (this); list.setRowHeight (54);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    list.setMultipleSelectionEnabled (true);
    addAndMakeVisible (list); addAndMakeVisible (importBtn); addAndMakeVisible (syncBtn);
    importBtn.setTooltip ("Import songs (MP3/WAV/FLAC/AIFF), video takes (MP4/MOV/ProRes/H.264/H.265), stills and .cube LUTs. Originals are never modified.");
    syncBtn.setTooltip ("Matches each take's own recorded audio to the song and places it in sync. Takes it can't place confidently are flagged orange, never guessed.");
    importBtn.onClick = [this] { if (onImport) onImport(); };
    syncBtn.onClick = [this] { if (onSyncAll) onSyncAll(); };
}

void MediaPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (30).withTrimmedTop (2);
    importBtn.setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (2, 0));
    syncBtn.setBounds (top.reduced (2, 0));
    r.removeFromTop (6);
    list.setBounds (r);
}

void MediaPanel::paint (juce::Graphics& g) { g.fillAll (theme::panel); }

int MediaPanel::getNumRows() { return (int) st.project.media.size(); }

void MediaPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= getNumRows()) return;
    auto& m = st.project.media[(size_t) row];
    g.fillAll (sel ? theme::accent.withAlpha (0.25f) : row % 2 ? theme::panel : theme::panel2.darker (0.2f));
    juce::Rectangle<int> th (4, 4, 82, h - 8);
    auto it = st.thumbs.find (m.id);
    if (it != st.thumbs.end() && it->second.isValid()) g.drawImage (it->second, th.toFloat(), juce::RectanglePlacement::centred);
    else
    {
        g.setColour (theme::line); g.fillRect (th);
        g.setColour (theme::dim); g.setFont (10.0f);
        g.drawText (m.kind == MediaKind::Audio ? "AUDIO" : m.kind == MediaKind::Image ? "IMAGE" : "VIDEO", th, juce::Justification::centred);
    }
    g.setColour (m.missing ? theme::bad : theme::text); g.setFont (12.0f);
    juce::String nm = m.name; if (m.id == st.project.songId) nm = juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xab ")) + nm;
    g.drawText (nm, 92, 4, w - 96, 16, juce::Justification::left, true);
    g.setColour (theme::dim); g.setFont (10.5f);
    juce::String sub = juce::String (ticksToSec (m.duration), 1) + " s";
    if (m.kind == MediaKind::Video) sub << "  " << m.width << "x" << m.height << "  " << juce::String (m.fps, 2) << " fps";
    if (m.missing) sub = "MISSING - right-click to locate";
    g.drawText (sub, 92, 20, w - 96, 14, juce::Justification::left, true);
    if (m.kind == MediaKind::Video && m.id != st.project.songId)
    {
        juce::String sy; juce::Colour c = theme::dim;
        switch (m.sync.status)
        {
            case SyncInfo::Synced:    sy = "synced " + juce::String (ticksToSec (m.sync.offset), 3) + " s  " + juce::String (juce::roundToInt (m.sync.confidence * 100)) + "%"; c = theme::good; break;
            case SyncInfo::Manual:    sy = "placed manually"; c = theme::good; break;
            case SyncInfo::Uncertain: sy = "uncertain - check placement"; c = theme::warn; break;
            case SyncInfo::Failed:    sy = "not synced: " + m.sync.note; c = theme::bad; break;
            case SyncInfo::None:      sy = m.hasAudio ? "not synced yet" : "no audio (place manually)"; break;
        }
        g.setColour (c); g.drawText (sy, 92, 34, w - 96, 14, juce::Justification::left, true);
    }
}

juce::var MediaPanel::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    if (rows.isEmpty() || rows[0] >= getNumRows()) return {};
    return "media:" + st.project.media[(size_t) rows[0]].id;
}

FxLibrary::FxLibrary (AppState& s) : juce::Thread ("ISFX fx examples"), st (s)
{
    open = { "T", "E", "S" };
    reload();
    list.setModel (this); list.setRowHeight (22);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    addAndMakeVisible (list);
    search.setTextToShowWhenEmpty ("Search effects, transitions, presets...", juce::Colours::grey);
    search.setColour (juce::TextEditor::backgroundColourId, theme::panel.darker (0.3f));
    search.onTextChange = [this] { refilter(); };
    search.onEscapeKey = [this] { search.clear(); refilter(); };
    addAndMakeVisible (search);
    addChildComponent (popBtn);
    popBtn.setTooltip ("Open the example in its own window you can move and resize (or dock it back).");
    popBtn.onClick = [this] { popOut (exWin == nullptr); };
    startThread (juce::Thread::Priority::low);
}

FxLibrary::~FxLibrary()
{
    exWin.reset();
    signalThreadShouldExit(); exWake.signal(); stopThread (4000);
    cancelPendingUpdate();
}

namespace
{
struct ExampleView : juce::Component
{
    explicit ExampleView (FxLibrary& f) : fx (f) {}
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); fx.paintExample (g, getLocalBounds().reduced (6)); }
    FxLibrary& fx;
};
struct ExampleWindow : juce::DocumentWindow
{
    ExampleWindow() : juce::DocumentWindow ("Effect Example", theme::panel, juce::DocumentWindow::allButtons) {}
    void closeButtonPressed() override { if (onClose) onClose(); }
    std::function<void()> onClose;
};
}

void FxLibrary::popOut (bool on)
{
    if (on && exWin == nullptr)
    {
        auto w = std::make_unique<ExampleWindow>();
        w->setUsingNativeTitleBar (true);
        w->setContentOwned (new ExampleView (*this), false);
        w->setResizable (true, false);
        w->setAlwaysOnTop (true);   // clicking back into the editor must not hide it behind the main window
        w->setResizeLimits (240, 160, 4000, 3000);
        w->centreWithSize (760, 460);
        juce::Component::SafePointer<FxLibrary> safe (this);
        w->onClose = [safe] { juce::MessageManager::callAsync ([safe] { if (safe) safe->popOut (false); }); };
        w->setVisible (true);
        exWin = std::move (w);
    }
    else if (! on) exWin.reset();
    popBtn.setButtonText (exWin ? "Dock" : "Pop out");
    resized(); repaint();
}

void FxLibrary::exRepaint()
{
    repaint (exArea);
    if (exWin != nullptr) if (auto* c = exWin->getContentComponent()) c->repaint();
}

void FxLibrary::resized()
{
    auto r = getLocalBounds().reduced (6).withTrimmedTop (20);
    exArea = {};
    if (exWin == nullptr && r.getHeight() > 150)
    {
        exArea = r.removeFromBottom (juce::jlimit (90, std::max (90, r.getHeight() - 60), juce::roundToInt (r.getHeight() * exFrac)));
        r.removeFromBottom (6);   // divider: drag to resize
        popBtn.setBounds (exArea.getRight() - 64, exArea.getY() + 6, 58, 15);
    }
    else popBtn.setBounds (getWidth() - 70, 3, 62, 16);
    popBtn.setVisible (exWin != nullptr || ! exArea.isEmpty());
    search.setBounds (r.removeFromTop (22)); r.removeFromTop (4);
    list.setBounds (r);
}

void FxLibrary::mouseDown (const juce::MouseEvent& e) { dragDiv = onDivider (e.y); }
void FxLibrary::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (onDivider (e.y) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}
void FxLibrary::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragDiv) return;
    auto inner = getLocalBounds().reduced (6).withTrimmedTop (20);
    exFrac = juce::jlimit (0.12f, 0.88f, (float) (inner.getBottom() - e.y) / (float) std::max (1, inner.getHeight()));
    resized(); repaint();
}

void FxLibrary::reload()
{
    all.clear();
    auto folder = [&] (int level, const juce::String& key, const juce::String& label)
    {
        Row r; r.label = label; r.header = true; r.level = level; r.folder = key;
        r.parent = key.containsChar ('/') ? key.upToLastOccurrenceOf ("/", false, false) : juce::String();
        all.push_back (r);
    };
    auto item = [&] (const juce::String& parent, const juce::String& label, const juce::String& desc, const juce::String& help)
    {
        Row r; r.label = label; r.desc = desc; r.help = help; r.parent = parent;
        r.level = parent.containsChar ('/') ? 2 : 1;
        all.push_back (r);
    };
    const auto cd = contentDir();
    juce::Array<juce::File> packs;
    if (cd.isDirectory()) { packs = cd.getChildFile ("Presets").findChildFiles (juce::File::findDirectories, false); packs.sort(); }
    auto isTransPack = [] (const juce::File& d) { return d.getFileName().containsIgnoreCase ("transition"); };
    auto addPack = [&] (const juce::File& d, const juce::String& top)
    {
        auto ps = loadUserPresets (d);
        if (ps.empty()) return;
        const auto key = top + "/pack:" + d.getFileName();
        folder (1, key, d.getFileName());
        for (auto& u : ps)
            item (key, u.name, "preset:" + u.file.getFullPathName(), "Stock preset from " + d.getFileName()
                  + ".\nDrop on a clip: applies its effects with their keyframes (IN/OUT presets line up with the clip start/end). Everything stays editable in the Inspector.");
    };

    folder (0, "T", "VIDEO TRANSITIONS");
    folder (1, "T/builtin", "Built-in (drag onto a cut)");
    auto tn = transitionNames();
    for (int i = 1; i < tn.size(); ++i)
        item ("T/builtin", tn[i], "transition:" + juce::String (i), "Drag onto the cut between two clips. It locks to that cut and moves with it. Set length, amount and direction in the Inspector.");
    for (auto& d : packs) if (isTransPack (d)) addPack (d, "T");

    folder (0, "E", "VIDEO EFFECTS");
    juce::StringArray cats;
    for (auto& l : lookLibrary()) cats.addIfNotAlreadyThere (l.category.isEmpty() ? juce::String ("Looks") : l.category);
    for (auto& cat : cats)
    {
        folder (1, "E/look:" + cat, cat);
        for (auto& l : lookLibrary())
            if ((l.category.isEmpty() ? juce::String ("Looks") : l.category) == cat)
                item ("E/look:" + cat, l.name, "look:" + l.name, l.help + "\nDrop on a clip for an effect layer over it, or on empty space for a 2-bar layer. Trim, move, fade (opacity) or disable it like any clip.");
    }
    for (auto& d : packs) if (! isTransPack (d)) addPack (d, "E");
    if (cd.isDirectory())
    {
        auto luts = cd.getChildFile ("LUTs").findChildFiles (juce::File::findFiles, true, "*.cube");
        luts.sort();
        if (! luts.isEmpty()) folder (1, "E/luts", "LUTs");
        for (auto& f : luts)
            item ("E/luts", f.getFileNameWithoutExtension(), "lut:" + f.getFullPathName(),
                  f.getParentDirectory().getFileName() + "\nDrop on a clip for a LUT layer over it, or on empty space for a 2-bar LUT layer. Fade it with opacity.");
    }
    bool head = false;
    for (auto& u : loadUserPresets())
        if (u.clip)
        {
            if (! head) { folder (1, "E/mine", "My Presets"); head = true; }
            item ("E/mine", u.name, "preset:" + u.file.getFullPathName(), "Drop on a clip: applies these effects with their keyframes. Everything stays editable in the Inspector.");
        }
    auto mine = userDataDir().getChildFile ("LUTs").findChildFiles (juce::File::findFiles, true, "*.cube");
    mine.sort();
    if (! mine.isEmpty()) folder (1, "E/myluts", "My LUTs");
    for (auto& f : mine) item ("E/myluts", f.getFileNameWithoutExtension(), "lut:" + f.getFullPathName(), "Imported LUT. Drop on a clip for a LUT layer over it.");

    if (cd.isDirectory())
    {
        auto sfx = cd.getChildFile ("SFX").findChildFiles (juce::File::findFiles, true, "*.mp3;*.wav;*.aif;*.aiff");
        sfx.sort();
        if (! sfx.isEmpty()) folder (0, "S", "SOUND FX");
        for (auto& f : sfx)
            item ("S", f.getFileNameWithoutExtension(), "sfx:" + f.getFullPathName(), "Drop on the timeline to add this sound at that spot.");
    }
    for (auto& r : all)   // item counts on folder rows
        if (r.header)
            for (auto& o : all)
                if (! o.header && (o.parent == r.folder || o.parent.startsWith (r.folder + "/"))) ++r.count;
    refilter();
}

void FxLibrary::refilter()
{
    auto shown = [&] (juce::String key)
    {
        while (key.isNotEmpty())
        {
            if (open.count (key) == 0) return false;
            key = key.containsChar ('/') ? key.upToLastOccurrenceOf ("/", false, false) : juce::String();
        }
        return true;
    };
    rows.clear();
    const auto q = search.getText().trim();
    if (q.isNotEmpty())
    {
        juce::StringArray words; words.addTokens (q, " ", ""); words.removeEmptyStrings();
        for (auto& r : all)
        {
            if (r.header) continue;
            bool hit = true;
            for (auto& w : words) hit = hit && (r.label.containsIgnoreCase (w) || r.parent.fromLastOccurrenceOf (":", false, false).containsIgnoreCase (w));
            if (hit) { auto x = r; x.level = 1; rows.push_back (x); }
        }
    }
    else
        for (auto& r : all) if (shown (r.parent)) rows.push_back (r);
    list.updateContent(); list.repaint();
}

void FxLibrary::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && row >= 0 && row < getNumRows() && ! rows[(size_t) row].header)
    {
        const auto desc = rows[(size_t) row].desc;
        if (desc.startsWith ("sfx:")) return;
        list.selectRow (row);
        juce::PopupMenu m;
        if (desc.startsWith ("transition:"))
        {
            const char* where[3] = { "Selected clips", "All clips", "Adjustment layer (fade)" };
            for (int sc = 0; sc < 3; ++sc)
            {
                juce::PopupMenu sub;
                sub.addItem (1 + sc * 10, "In (start of clip)");
                sub.addItem (2 + sc * 10, "Out (end of clip)");
                sub.addItem (3 + sc * 10, "In + Out");
                m.addSubMenu (juce::String ("Apply to ") + where[sc], sub);
            }
        }
        else
        {
            m.addItem (1, "Apply to selected clips");
            m.addItem (11, "Apply to all clips");
            m.addItem (21, "Apply to adjustment layer (selected, or a new one over the selection)");
        }
        juce::Component::SafePointer<FxLibrary> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, desc] (int r)
        {
            if (r <= 0 || safe == nullptr || ! safe->onApply) return;
            safe->onApply (desc, (r - 1) / 10, (r - 1) % 10);
        });
        return;
    }
    if (row < 0 || row >= getNumRows() || ! rows[(size_t) row].header) return;
    const auto key = rows[(size_t) row].folder;
    if (open.count (key)) open.erase (key); else open.insert (key);
    refilter();
    list.selectRow (row, true, true);
}

void FxLibrary::selectedRowsChanged (int row)
{
    if (row < 0 || row >= getNumRows()) return;
    auto& r = rows[(size_t) row];
    if (r.header) return;
    {
        const juce::ScopedLock sl (exLock);
        exWant = r.desc; exName = r.label;
        exProj = std::make_shared<const Project> (st.project);
        exAt = st.playhead;
        exNote = "Rendering example...";
    }
    exWake.signal();
    exRepaint();
}

void FxLibrary::run()
{
    std::vector<juce::uint8> px;
    while (! threadShouldExit())
    {
        exWake.wait (200);
        juce::String desc; std::shared_ptr<const Project> p; Ticks at = 0;
        {
            const juce::ScopedLock sl (exLock);
            if (exWant.isEmpty()) continue;
            desc = exWant; exWant = {}; p = exProj; at = exAt;
        }
        if (p == nullptr) continue;
        const auto& seq = p->seq();
        auto isVideo = [&] (const Clip& c)
        {
            if (! c.enabled || c.mediaId.isEmpty() || c.track < 0 || c.track >= (int) seq.tracks.size()) return false;
            if (seq.tracks[(size_t) c.track].kind != TrackKind::Video) return false;
            auto* m = p->findMedia (c.mediaId);
            return m != nullptr && m->kind != MediaKind::Audio;
        };
        juce::Image out; juce::String note;
        int W = 0, H = 0; outputSize (*p, 360, W, H);
        if (desc.startsWith ("sfx:")) note = "Sound effect: drag it onto the timeline to hear it.";
        else if (desc.startsWith ("transition:"))
        {   // the transition on a real cut of this edit: 3 frames across it
            const Clip* best = nullptr; Ticks bestD = 0;
            for (auto& c : seq.clips)
            {
                if (! isVideo (c)) continue;
                bool cut = false;
                for (auto& o : seq.clips)
                    if (&o != &c && o.track == c.track && isVideo (o) && std::llabs (o.end() - c.start) <= secToTicks (0.05)) { cut = true; break; }
                const Ticks d = std::llabs (c.start - at);
                if (cut && (best == nullptr || d < bestD)) { best = &c; bestD = d; }
            }
            if (best == nullptr) note = "Put two clips back to back on a track to see this transition on your footage.";
            else
            {
                Sequence s = seq;
                auto* k = s.findClip (best->id);
                k->transIn = (TransitionType) desc.fromFirstOccurrenceOf (":", false, false).getIntValue();
                k->transLen = std::min (secToTicks (0.6), k->length / 2);
                const int fw = std::max (1, W / 2), fh = std::max (1, H / 2);
                out = juce::Image (juce::Image::ARGB, fw * 3 + 4, fh, true);
                juce::Graphics g (out);
                const double at3[] = { 0.2, 0.5, 0.8 };
                for (int i = 0; i < 3 && ! threadShouldExit(); ++i)
                    if (exRenderer.render (*p, s, k->start + (Ticks) (k->transLen * at3[i]), fw, fh, px, 360))
                        g.drawImageAt (rgbaToImage (px, fw, fh), i * (fw + 2), 0);
            }
        }
        else
        {   // before | after on the clip under the playhead (or the first clip)
            const Clip* c = nullptr;
            for (auto& o : seq.clips) if (isVideo (o) && at >= o.start && at < o.end()) { c = &o; break; }
            if (c == nullptr) for (auto& o : seq.clips) if (isVideo (o)) { c = &o; break; }
            if (c == nullptr) note = "Add a clip to the timeline to see examples on your footage.";
            else
            {
                Ticks t = at >= c->start && at < c->end() ? at : c->start + c->length / 2;
                Sequence s = seq;
                bool ok = true;
                if (desc.startsWith ("look:"))
                {
                    const auto name = desc.fromFirstOccurrenceOf (":", false, false);
                    ok = false;
                    for (auto& l : lookLibrary())
                        if (l.name == name) { FxStack fx; applyLook (fx, l, 1.0f); addEffectClip (s, fx, l.name, c->start, c->length); ok = true; break; }
                }
                else if (desc.startsWith ("lut:"))
                {
                    FxStack fx; fx.lutPath = desc.fromFirstOccurrenceOf (":", false, false);
                    addEffectClip (s, fx, "LUT", c->start, c->length);
                }
                else if (desc.startsWith ("preset:"))
                {
                    UserPreset up; juce::String err;
                    ok = readPresetFile (juce::File (desc.fromFirstOccurrenceOf ("preset:", false, false)), up, err);
                    if (ok)
                        if (auto* k = s.findClip (c->id))
                        {
                            applyUserPreset (*k, up);
                            if (up.length > 0)
                            {   // middle of the preset's animation
                                const Ticks span = std::min (k->length, up.length);
                                t = up.atEnd ? k->end() - span / 2 : k->start + span / 2;
                            }
                        }
                }
                if (! ok) note = "No example for this item.";
                else
                {
                    std::vector<juce::uint8> before;
                    const bool b = exRenderer.render (*p, seq, t, W, H, before, 360);
                    const bool a = exRenderer.render (*p, s, t, W, H, px, 360);
                    if (a && b)
                    {   // left half untouched, right half with the effect
                        for (int y = 0; y < H; ++y)
                            std::memcpy (px.data() + (size_t) y * W * 4, before.data() + (size_t) y * W * 4, (size_t) (W / 2) * 4);
                        out = rgbaToImage (px, W, H);
                        juce::Graphics g (out);
                        g.setColour (juce::Colours::white.withAlpha (0.8f));
                        g.drawVerticalLine (W / 2, 0.0f, (float) H);
                        g.setFont (11.0f);
                        g.drawText ("Before", 4, H - 16, W / 2 - 8, 14, juce::Justification::left);
                        g.drawText ("After", W / 2 + 4, H - 16, W / 2 - 8, 14, juce::Justification::right);
                    }
                    else note = "Could not render the example.";
                }
            }
        }
        {
            const juce::ScopedLock sl (exLock);
            if (exWant.isEmpty()) { example = out; exNote = note; }
        }
        triggerAsyncUpdate();
    }
}

void FxLibrary::handleAsyncUpdate() { exRepaint(); }

void FxLibrary::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::accent2); g.setFont (headerFont (10.0f));
    g.drawText ("EFFECTS", 10, 4, getWidth() - 20, 16, juce::Justification::centredLeft);
    if (exArea.isEmpty()) return;
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.fillRoundedRectangle ((float) exArea.getCentreX() - 18, (float) exArea.getY() - 5, 36, 3, 1.5f);   // divider grip
    paintExample (g, exArea);
}

void FxLibrary::paintExample (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area.reduced (0, 4);
    g.setColour (theme::panel2.darker (0.3f)); g.fillRoundedRectangle (r.toFloat(), 4.0f);
    juce::Image im; juce::String note, name;
    { const juce::ScopedLock sl (exLock); im = example; note = exNote; name = exName; }
    auto top = r.removeFromTop (16);
    g.setColour (theme::accent); g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.drawText (name.isEmpty() ? juce::String ("EXAMPLE: select an effect or transition") : "EXAMPLE: " + name, top.reduced (6, 0).withTrimmedRight (exWin ? 0 : 64), juce::Justification::centredLeft, true);
    r = r.reduced (6, 4);
    if (im.isValid() && note.isEmpty())
        g.drawImageWithin (im, r.getX(), r.getY(), r.getWidth(), r.getHeight(), juce::RectanglePlacement::centred);
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.6f)); g.setFont (11.5f);
        g.drawFittedText (note, r, juce::Justification::centred, 3);
    }
}

void FxLibrary::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= getNumRows()) return;
    auto& r = rows[(size_t) row];
    const int x = 8 + r.level * 12;
    if (r.header)
    {
        g.fillAll (r.level == 0 ? theme::panel2 : theme::panel2.darker (0.1f));
        const bool isOpen = open.count (r.folder) > 0;
        juce::Path tri;
        const float cx = (float) x + 4, cy = h * 0.5f;
        if (isOpen) tri.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        else tri.addTriangle (cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy);
        g.setColour (theme::accent); g.fillPath (tri);
        // folder glyph
        g.setColour (theme::accent2.withAlpha (0.8f));
        g.fillRoundedRectangle ((float) x + 12, cy - 5, 12, 10, 1.5f);
        g.setColour (r.level == 0 ? theme::accent : juce::Colours::white.withAlpha (0.88f));
        g.setFont (juce::FontOptions (r.level == 0 ? 10.5f : 12.0f, juce::Font::bold));
        g.drawText (r.label + "  (" + juce::String (r.count) + ")", x + 30, 0, w - x - 34, h, juce::Justification::centredLeft, true);
        return;
    }
    g.fillAll (sel ? theme::accent.withAlpha (0.25f) : row % 2 ? theme::panel : theme::panel2.darker (0.2f));
    g.setColour (juce::Colours::white.withAlpha (0.9f)); g.setFont (12.5f);
    g.drawText (r.label, x + 14, 0, w - x - 18, h, juce::Justification::centredLeft, true);
}

juce::var FxLibrary::getDragSourceDescription (const juce::SparseSet<int>& sel)
{
    if (sel.isEmpty() || sel[0] >= getNumRows() || rows[(size_t) sel[0]].header) return {};
    return rows[(size_t) sel[0]].desc;
}

juce::String FxLibrary::getTooltipForRow (int row)
{
    if (row < 0 || row >= getNumRows()) return {};
    auto& r = rows[(size_t) row];
    return r.header ? "Click to open or close this folder." : r.help + "\nClick to see an example on your footage below.";
}

void MediaPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < getNumRows() && onAddToTimeline) onAddToTimeline (st.project.media[(size_t) row].id);
}

juce::String MediaPanel::getTooltipForRow (int row)
{
    if (row < 0 || row >= getNumRows()) return {};
    auto& m = st.project.media[(size_t) row];
    return m.path + "\nDrag to the timeline or double-click to add at the playhead. Right-click for more.";
}

void MediaPanel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || row < 0 || row >= getNumRows()) return;
    auto id = st.project.media[(size_t) row].id;
    auto& m = st.project.media[(size_t) row];
    juce::PopupMenu menu;
    menu.addItem (1, "Add to timeline at playhead");
    menu.addItem (2, "Use as the song", m.kind != MediaKind::Image);
    menu.addItem (3, "Re-sync this take", m.kind == MediaKind::Video && m.hasAudio && st.project.song.valid);
    menu.addItem (4, m.missing ? "Locate missing file..." : "Relink to another file...");
    menu.addSeparator();
    menu.addItem (5, "Remove from project");
    menu.showMenuAsync (juce::PopupMenu::Options(), [this, id] (int r)
    {
        if (r == 1 && onAddToTimeline) onAddToTimeline (id);
        if (r == 2 && onSetSong) onSetSong (id);
        if (r == 3 && onResync) onResync (id);
        if (r == 4 && onRelink) onRelink (id);
        if (r == 5 && onRemove) onRemove (id);
    });
}

// ============================================================ Inspector
namespace
{
struct ParamRow : public juce::Component
{
    juce::Label name; juce::Slider slider;
    ParamRow (const juce::String& n, const juce::String& help, double lo, double hi, double v, double step)
    {
        name.setText (n, juce::dontSendNotification); name.setFont (12.0f); name.setTooltip (help);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
        slider.setRange (lo, hi, step); slider.setValue (v, juce::dontSendNotification);
        slider.setTooltip (help); slider.setDoubleClickReturnValue (true, v);
        addAndMakeVisible (name); addAndMakeVisible (slider);
    }
    void resized() override { auto r = getLocalBounds(); name.setBounds (r.removeFromLeft (110)); slider.setBounds (r); }
};
struct ComboRow : public juce::Component
{
    juce::Label l; juce::ComboBox cb;
    ComboRow (const juce::String& label, const juce::StringArray& opts, int sel, const juce::String& help)
    {
        l.setText (label, juce::dontSendNotification); l.setFont (12.0f); l.setTooltip (help);
        cb.addItemList (opts, 1); cb.setSelectedItemIndex (sel, juce::dontSendNotification); cb.setTooltip (help);
        addAndMakeVisible (l); addAndMakeVisible (cb);
    }
    void resized() override { auto r = getLocalBounds(); l.setBounds (r.removeFromLeft (110)); cb.setBounds (r.reduced (0, 2)); }
};
struct Heading : public juce::Component
{
    juce::String t; explicit Heading (juce::String s) : t (std::move (s)) {}
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::accent2); g.setFont (juce::Font (11.0f, juce::Font::bold));
        g.drawText (t.toUpperCase(), getLocalBounds().withTrimmedTop (6), juce::Justification::left);
        g.setColour (theme::line); g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }
};
}

Inspector::Inspector (AppState& s) : st (s)
{
    title.setFont (juce::Font (14.0f, juce::Font::bold));
    addAndMakeVisible (title);
    view.setViewedComponent (&content, false);
    view.setScrollBarsShown (true, false);
    addAndMakeVisible (view);
    rebuild();
}

void Inspector::paint (juce::Graphics& g) { g.fillAll (theme::panel); }

// CapCut-style tone curves: RGB master plus R, G, B; five draggable points per channel.
struct CurveEditor : public juce::Component
{
    CurveEditor (std::function<FxStack&()> s, std::function<void()> b, std::function<void()> e)
        : stack (std::move (s)), begin (std::move (b)), edited (std::move (e)) {}
    static juce::String pid (int c, int i) { return juce::String ("crv") + juce::String::charToString ("MRGB"[c]) + juce::String (i); }
    float val (int c, int i) const { return stack().get (pid (c, i), (float) i / 4.0f); }
    juce::Rectangle<float> plot() const { auto r = getLocalBounds().toFloat().reduced (4); r.removeFromTop (26); return r; }
    float eval (int c, float x) const
    {
        float y[5]; for (int i = 0; i < 5; ++i) y[i] = val (c, i);
        float f = juce::jlimit (0.0f, 1.0f, x) * 4; int i = std::min (3, (int) f); float u = f - (float) i;
        float p1 = y[i], p2 = y[i + 1], p0 = i > 0 ? y[i - 1] : 2 * p1 - p2, p3 = i < 3 ? y[i + 2] : 2 * p2 - p1;
        return juce::jlimit (0.0f, 1.0f, 0.5f * (2 * p1 + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (-p0 + 3 * p1 - 3 * p2 + p3) * u * u * u));
    }
    static juce::Colour colourFor (int c)
    {
        return c == 0 ? juce::Colours::white : c == 1 ? juce::Colour (0xffff5a5a) : c == 2 ? juce::Colour (0xff4ade80) : juce::Colour (0xff5aa9ff);
    }
    void paint (juce::Graphics& g) override
    {
        const char* names[4] = { "RGB", "R", "G", "B" };
        for (int c = 0; c < 4; ++c)
        {
            juce::Rectangle<float> tab (4.0f + (float) c * 46.0f, 4.0f, 42.0f, 20.0f);
            g.setColour (c == ch ? colourFor (c).withAlpha (0.25f) : theme::panel2); g.fillRoundedRectangle (tab, 4);
            g.setColour (c == ch ? colourFor (c) : theme::dim); g.setFont (11.0f);
            g.drawText (names[c], tab, juce::Justification::centred);
        }
        g.setColour (theme::dim); g.setFont (10.0f);
        g.drawText ("double-click resets", getLocalBounds().removeFromTop (28).reduced (6, 0), juce::Justification::centredRight);
        auto p = plot();
        g.setColour (juce::Colour (0xff0b0c10)); g.fillRoundedRectangle (p, 4);
        g.setColour (theme::line);
        for (int i = 1; i < 4; ++i)
        {
            g.drawVerticalLine ((int) (p.getX() + p.getWidth() * (float) i / 4), p.getY(), p.getBottom());
            g.drawHorizontalLine ((int) (p.getY() + p.getHeight() * (float) i / 4), p.getX(), p.getRight());
        }
        g.drawLine (p.getX(), p.getBottom(), p.getRight(), p.getY(), 0.6f);
        for (int pass = 0; pass < 2; ++pass)
            for (int c = 0; c < 4; ++c)
            {
                if ((pass == 1) != (c == ch)) continue;
                juce::Path path;
                for (int k = 0; k <= 64; ++k)
                {
                    float x = (float) k / 64, y = eval (c, x);
                    juce::Point<float> pt (p.getX() + x * p.getWidth(), p.getBottom() - y * p.getHeight());
                    if (k == 0) path.startNewSubPath (pt); else path.lineTo (pt);
                }
                g.setColour (colourFor (c).withAlpha (c == ch ? 1.0f : 0.22f));
                g.strokePath (path, juce::PathStrokeType (c == ch ? 1.8f : 1.0f));
            }
        for (int i = 0; i < 5; ++i)
        {
            juce::Point<float> pt (p.getX() + p.getWidth() * (float) i / 4, p.getBottom() - val (ch, i) * p.getHeight());
            g.setColour (juce::Colour (0xff0b0c10)); g.fillEllipse (juce::Rectangle<float> (10, 10).withCentre (pt));
            g.setColour (colourFor (ch)); g.drawEllipse (juce::Rectangle<float> (10, 10).withCentre (pt), 1.6f);
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.y < 28) { ch = juce::jlimit (0, 3, (e.x - 4) / 46); dragIdx = -1; repaint(); return; }
        auto p = plot();
        dragIdx = juce::jlimit (0, 4, (int) std::lround ((e.position.x - p.getX()) / p.getWidth() * 4));
        if (begin) begin();
        setFromMouse (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (dragIdx >= 0) setFromMouse (e); }
    void mouseUp (const juce::MouseEvent&) override { dragIdx = -1; }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < 28) return;
        if (begin) begin();
        for (int i = 0; i < 5; ++i) stack().set (pid (ch, i), (float) i / 4.0f);
        if (edited) edited();
        repaint();
    }
    void setFromMouse (const juce::MouseEvent& e)
    {
        auto p = plot();
        stack().set (pid (ch, dragIdx), juce::jlimit (0.0f, 1.0f, 1 - (e.position.y - p.getY()) / p.getHeight()));
        if (edited) edited();
        repaint();
    }
    std::function<FxStack&()> stack;
    std::function<void()> begin, edited;
    int ch = 0, dragIdx = -1;
};

// speed / time-remap graph: x = clip time, y = speed (0..4x); click adds a point, drag moves, double-click deletes,
// right-click cycles Linear / Ease / Hold. Points are the clip's "speed" keys, so playback, split and export follow it.
struct SpeedGraph : public juce::Component
{
    SpeedGraph (std::function<Clip&()> c, std::function<void()> b, std::function<void()> e)
        : clip (std::move (c)), begin (std::move (b)), edited (std::move (e)) {}
    static constexpr float maxV = 4.0f;
    juce::Rectangle<float> plot() const { auto r = getLocalBounds().toFloat().reduced (4); r.removeFromTop (16); return r; }
    float xOf (Ticks t) const { auto r = plot(); return r.getX() + r.getWidth() * (float) ((double) t / std::max<Ticks> (1, clip().length)); }
    float yOf (float v) const { auto r = plot(); return r.getBottom() - r.getHeight() * juce::jlimit (0.0f, 1.0f, v / maxV); }
    Ticks tOf (float x) const { auto r = plot(); return (Ticks) ((double) juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth()) * (double) clip().length); }
    float vOf (float y) const { auto r = plot(); return juce::jlimit (0.1f, maxV, (r.getBottom() - y) / r.getHeight() * maxV); }
    void paint (juce::Graphics& g) override
    {
        auto r = plot();
        g.setColour (theme::text.withAlpha (0.8f)); g.setFont (11.0f);
        g.drawText ("Speed graph (time remap): click add, drag, dbl-click delete, right-click ease", getLocalBounds().removeFromTop (18).reduced (4, 0), juce::Justification::left);
        g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRect (r);
        g.setColour (theme::line); g.drawRect (r);
        g.setColour (theme::line.brighter (0.3f)); g.drawHorizontalLine ((int) yOf (1.0f), r.getX(), r.getRight());
        const auto& c = clip();
        auto it = c.keys.find ("speed");
        const bool has = it != c.keys.end() && ! it->second.empty();
        juce::Path p;
        for (int i = 0; i <= 120; ++i)
        {
            const Ticks t = (Ticks) ((double) c.length * i / 120.0);
            const float v = has ? keyValue (it->second, t) : (float) c.speed;
            const float x = xOf (t), y = yOf (v);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (theme::accent2); g.strokePath (p, juce::PathStrokeType (2.0f));
        if (has)
            for (auto& k : it->second)
            {
                g.setColour (k.interp == 2 ? theme::accent2 : k.interp == 1 ? juce::Colours::orange : juce::Colours::white);
                g.fillEllipse (xOf (k.t) - 4, yOf (k.v) - 4, 8, 8);
            }
    }
    int hit (juce::Point<float> pos) const
    {
        auto it = clip().keys.find ("speed");
        if (it == clip().keys.end()) return -1;
        for (int i = 0; i < (int) it->second.size(); ++i)
            if (pos.getDistanceFrom ({ xOf (it->second[(size_t) i].t), yOf (it->second[(size_t) i].v) }) < 7) return i;
        return -1;
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        begin();
        auto& c = clip();
        auto& ks = c.keys["speed"];
        if (ks.empty())
        {
            Clip::Key a; a.t = 0; a.v = (float) c.speed; a.interp = 2;
            Clip::Key b = a; b.t = c.length;
            ks = { a, b };
        }
        const int h = hit (e.position);
        if (e.mods.isPopupMenu()) { if (h >= 0) ks[(size_t) h].interp = (ks[(size_t) h].interp + 1) % 3 == 0 ? 0 : (ks[(size_t) h].interp == 0 ? 2 : 1); edited(); repaint(); return; }
        if (h >= 0) { drag = h; return; }
        Clip::Key k; k.t = tOf (e.position.x); k.v = vOf (e.position.y); k.interp = 2;
        auto pos = std::upper_bound (ks.begin(), ks.end(), k.t, [] (Ticks x, const Clip::Key& y) { return x < y.t; });
        drag = (int) (ks.insert (pos, k) - ks.begin());
        edited(); repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& ks = clip().keys["speed"];
        if (drag < 0 || drag >= (int) ks.size()) return;
        auto& k = ks[(size_t) drag];
        k.v = vOf (e.position.y);
        if (drag > 0 && drag < (int) ks.size() - 1)
            k.t = juce::jlimit (ks[(size_t) drag - 1].t + 1, std::max (ks[(size_t) drag - 1].t + 1, ks[(size_t) drag + 1].t - 1), tOf (e.position.x));
        edited(); repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { drag = -1; }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto& ks = clip().keys["speed"];
        const int h = hit (e.position);
        if (h > 0 && h < (int) ks.size() - 1) { begin(); ks.erase (ks.begin() + h); edited(); repaint(); }
    }
    std::function<Clip&()> clip; std::function<void()> begin, edited;
    int drag = -1;
};

struct TextRow : public juce::Component
{
    juce::Label l; juce::TextEditor ed;
    TextRow (const juce::String& label, const juce::String& v)
    {
        l.setText (label, juce::dontSendNotification); l.setFont (12.0f);
        ed.setText (v, juce::dontSendNotification);
        addAndMakeVisible (l); addAndMakeVisible (ed);
    }
    void resized() override { auto r = getLocalBounds(); l.setBounds (r.removeFromLeft (110)); ed.setBounds (r.reduced (0, 2)); }
};

void Inspector::resized()
{
    auto r = getLocalBounds().reduced (8);
    title.setBounds (r.removeFromTop (24));
    view.setBounds (r);
    int w = view.getWidth() - view.getScrollBarThickness() - 2, y = 0;
    for (auto* c : items) { int h = dynamic_cast<Heading*> (c) ? 24 : dynamic_cast<CurveEditor*> (c) ? 236 : dynamic_cast<SpeedGraph*> (c) ? 140 : 26; c->setBounds (0, y, w, h); y += h; }
    content.setSize (w, y + 10);
}

void Inspector::rebuild()
{
    items.clear(); content.removeAllChildren();
    auto& seq = st.project.seq();
    // with several clips selected the Inspector shows the first one and every change goes to all of them
    Clip* clip = st.selected.isEmpty() ? nullptr : seq.findClip (st.selected[0]);
    const juce::StringArray allSel = st.selected;
    const bool showText = clip != nullptr && clip->text.isNotEmpty();
    target = clip ? clip->id : juce::String();
    auto add = [this] (juce::Component* c) { items.add (c); content.addAndMakeVisible (c); };
    auto edited = [this] { st.dirty = true; if (onEdited) onEdited(); };

    auto addCombo = [&] (const juce::String& label, const juce::StringArray& opts, int sel, const juce::String& help, std::function<void (int)> fn)
    {
        auto* row = new ComboRow (label, opts, sel, help);
        row->cb.onChange = [row, fn] { fn (row->cb.getSelectedItemIndex()); };
        add (row);
    };

    auto addParams = [&] (const std::vector<ParamDef>& defs, std::function<FxStack&()> stack, bool multi = false)
    {
        juce::String group;
        bool curves = false;
        for (auto& d : defs)
        {
            if (juce::String (d.group) == "Curves")
            {
                if (! curves) { curves = true; group = d.group; add (new Heading ("Curves")); add (new CurveEditor (stack, [this] { st.edit ("Curve"); }, edited)); }
                continue;
            }
            if (juce::String (d.group) == "Text" && ! showText) continue;
            if (juce::String (d.group) != group) { group = d.group; add (new Heading (group)); }
            float v = stack().get (d.id, d.def);
            bool discrete = juce::String (d.id) == "reframe" || juce::String (d.id) == "blend" || juce::String (d.id) == "maskShape" || juce::String (d.id) == "maskInvert";
            auto* r = new ParamRow (d.name, d.help, d.min, d.max, v, discrete ? 1.0 : (d.max - d.min) / 1000.0);
            juce::String id (d.id);
            if (id == "blend") r->slider.textFromValueFunction = [] (double x) { return blendModeNames()[(int) x]; };
            if (id == "reframe") r->slider.textFromValueFunction = [] (double x) { return juce::StringArray { "Fill", "Fit", "Blur" }[(int) x]; };
            if (id == "maskShape") r->slider.textFromValueFunction = [] (double x) { return juce::StringArray { "None", "Ellipse", "Rect" }[(int) x]; };
            r->slider.updateText();
            r->slider.onDragStart = [this, n = juce::String (d.name)] { st.edit (n); };
            r->slider.onValueChange = [this, r, id, stack, edited, multi, allSel]
            {
                if (! r->slider.isMouseButtonDown()) st.edit (id);
                const float v = (float) r->slider.getValue();
                stack().set (id, v);
                if (multi && allSel.size() > 1)
                    for (auto& sid : allSel) if (auto* k = st.project.seq().findClip (sid); k && ! k->locked) k->fx.set (id, v);
                edited();
            };
            add (r);
        }
    };

    if (clip)
    {
        auto* m = st.project.findMedia (clip->mediaId);
        title.setText (allSel.size() > 1 ? juce::String (allSel.size()) + " clips (changes apply to all)"
                       : clip->text.isNotEmpty() ? "Text: " + clip->text
                       : "Clip: " + (m ? m->name : clip->name.isNotEmpty() ? clip->name : juce::String ("?")), juce::dontSendNotification);
        const juce::String cid = clip->id;
        auto clipRef = [this, cid]() -> Clip& { static Clip dummy; auto* c = st.project.seq().findClip (cid); return c ? *c : dummy; };
        add (new Heading ("Clip"));
        auto mk = [&] (const juce::String& n, const juce::String& help, double lo, double hi, double v, std::function<void (Clip&, double)> set)
        {
            auto* r = new ParamRow (n, help, lo, hi, v, (hi - lo) / 1000.0);
            r->slider.onDragStart = [this, n] { st.edit (n); };
            r->slider.onValueChange = [this, r, set, clipRef, edited, n, allSel]
            {
                if (! r->slider.isMouseButtonDown()) st.edit (n);
                set (clipRef(), r->slider.getValue());
                for (auto& sid : allSel) if (auto* k = st.project.seq().findClip (sid); k && ! k->locked && k->id != clipRef().id) set (*k, r->slider.getValue());
                edited();
            };
            add (r);
        };
        if (clip->text.isNotEmpty())
        {
            auto* tr = new TextRow ("Text", clip->text);
            auto commit = [this, tr, clipRef, edited]
            {
                auto v = tr->ed.getText().trim();
                if (v.isEmpty() || v == clipRef().text) return;
                st.edit ("Text"); clipRef().text = v; clipRef().name = v; edited();
            };
            tr->ed.onReturnKey = commit; tr->ed.onFocusLost = commit;
            add (tr);
            auto fonts = juce::Font::findAllTypefaceNames(); fonts.insert (0, "(default)");
            addCombo ("Font", fonts, std::max (0, fonts.indexOf (clip->font)), "Typeface for this text.",
                      [this, clipRef, edited, fonts] (int i) { st.edit ("Font"); clipRef().font = i <= 0 ? juce::String() : fonts[i]; edited(); });
        }
        mk ("Opacity", "Clip opacity for compositing over lower tracks.", 0, 1, clip->opacity, [] (Clip& c, double v) { c.opacity = (float) v; });
        mk ("Volume", "Clip audio level (video takes start muted; the song carries the mix).", 0, 2, clip->volume, [] (Clip& c, double v) { c.volume = (float) v; });
        mk ("Speed", "Playback speed. Values below 1 slow motion, above 1 speed up. Negative values are not used; use Reverse.", 0.1, 4, clip->speed, [] (Clip& c, double v) { c.speed = v; });
        if (m != nullptr && m->kind == MediaKind::Video)
        {
            add (new Heading ("Speed ramp / time remap"));
            add (new SpeedGraph (clipRef, [this] { st.edit ("Speed graph"); }, edited));
        }
        addCombo ("Transition in", transitionNames(), (int) clip->transIn, "Transition from the previous clip on this track.",
                  [this, clipRef, edited] (int i) { st.edit ("Transition"); auto& c = clipRef(); c.transIn = (TransitionType) i;
                      if (c.transLen == 0 && i > 0) c.transLen = secToTicks (st.project.song.valid ? st.project.song.beatSec() / 2 : 0.25); edited(); });
        mk ("Transition beats", "Transition length in beats.", 0, 4, st.project.song.valid ? ticksToSec (clip->transLen) / st.project.song.beatSec() : ticksToSec (clip->transLen) * 2,
            [this] (Clip& c, double v) { c.transLen = secToTicks (v * (st.project.song.valid ? st.project.song.beatSec() : 0.5)); });
        mk ("Transition amount", "How hard the transition hits: distance, spin, blur, split, burn glow.", 0, 2, clip->fx.get ("trAmt", 1.0f),
            [] (Clip& c, double v) { c.fx.set ("trAmt", (float) v); });
        addCombo ("Transition direction", { "Left / In", "Right / Out", "Up", "Down" }, juce::jlimit (0, 3, (int) clip->fx.get ("trDir", 0.0f)),
                  "Direction for whip, spin, wipe, mirror and light leak (Zoom: In or Out; Luma Wipe: bright-first or dark-first).",
                  [this, clipRef, edited] (int i) { st.edit ("Transition direction"); clipRef().fx.set ("trDir", (float) i); edited(); });
        juce::StringArray cps { "(choose a preset)" }; std::vector<UserPreset> cpv;
        for (auto& u : loadUserPresets()) if (u.clip) { cpv.push_back (u); cps.add (u.name); }
        addCombo ("Clip preset", cps, 0, "Your saved clip effects (Presets > Save clip effects).",
                  [this, clipRef, edited, cpv] (int i) { if (i <= 0) return; st.edit ("Clip preset"); clipRef().fx = cpv[(size_t) i - 1].fx; edited();
                      juce::MessageManager::callAsync ([this] { rebuild(); resized(); }); });
        if (clip->mediaId.isEmpty())
        {
            title.setText ("Adjustment layer: " + (clip->name.isNotEmpty() ? clip->name : juce::String ("FX")), juce::dontSendNotification);
            add (new Heading ("Adjustment layer (affects every track under it)"));
            addParams (sequenceParams(), [clipRef]() -> FxStack& { return clipRef().fx; });
        }
        else
            addParams (clipParams(), [clipRef]() -> FxStack& { return clipRef().fx; }, true);
    }
    else
    {
        title.setText ("Sequence look: " + seq.name, juce::dontSendNotification);
        add (new Heading ("Look library"));
        juce::StringArray looks { "(choose a look)" };
        for (auto& l : lookLibrary()) looks.add (l.category + " / " + l.name);
        std::vector<UserPreset> ups; for (auto& u : loadUserPresets()) if (! u.clip) { ups.push_back (u); looks.add ("My Presets / " + u.name); }
        addCombo ("Look", looks, 0, "Applies a complete grade + FX stack to the whole sequence. Every slider below stays editable.",
                  [this, edited, ups] (int i) { if (i <= 0) return; st.edit ("Look"); auto lut = st.project.seq().fx.lutPath;
                      const int nl = (int) lookLibrary().size();
                      if (i - 1 < nl) { st.project.seq().fx.p.clear(); st.project.seq().fx.lutPath = lut; applyLook (st.project.seq().fx, lookLibrary()[(size_t) i - 1], 1.0f); }
                      else st.project.seq().fx = ups[(size_t) (i - 1 - nl)].fx;
                      edited();
                      juce::MessageManager::callAsync ([this] { rebuild(); resized(); }); });
        juce::StringArray modes { "(none)" }; modes.addArray (styleModes());
        addCombo ("Style mode", modes, 0, "Classic, Gothic, VHS, Y2K, Underground, Abstract: a base treatment layered under the look.",
                  [this, edited] (int i) { if (i <= 0) return; st.edit ("Style mode"); applyStyleMode (st.project.seq().fx, styleModes()[i - 1]); edited();
                      juce::MessageManager::callAsync ([this] { rebuild(); resized(); }); });
        addCombo ("LUT", { seq.fx.lutPath.isEmpty() ? "(none)" : juce::File (seq.fx.lutPath).getFileName(), "Load .cube...", "Clear" }, 0,
                  "3D LUT (.cube) applied in the grade; LUT Mix sets the intensity.",
                  [this, edited] (int i)
                  {
                      if (i == 2) { st.edit ("Clear LUT"); st.project.seq().fx.lutPath = {}; edited(); juce::MessageManager::callAsync ([this] { rebuild(); resized(); }); }
                      if (i != 1) return;
                      auto fc = std::make_shared<juce::FileChooser> ("Choose a .cube LUT", juce::File(), "*.cube");
                      fc->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, fc, edited] (const juce::FileChooser& f)
                      {
                          auto file = f.getResult();
                          if (file.existsAsFile())
                          {
                              Lut3D l; juce::String err;
                              if (! l.load (file, err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "LUT", err); return; }
                              st.edit ("Load LUT"); st.project.seq().fx.lutPath = file.getFullPathName();
                              if (st.project.seq().fx.get ("lutMix", 0) <= 0) st.project.seq().fx.set ("lutMix", 1.0f);
                              edited();
                          }
                          rebuild(); resized();
                      });
                  });
        addParams (sequenceParams(), [this]() -> FxStack& { return st.project.seq().fx; });
    }
    resized();
}
}
