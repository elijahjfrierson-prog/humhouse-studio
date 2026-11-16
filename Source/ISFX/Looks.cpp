#include "Looks.h"

namespace isfx
{
const std::vector<ParamDef>& sequenceParams()
{
    static const std::vector<ParamDef> d {
        { "exposure",   "Exposure",    -2, 2, 0, "Color", "Overall brightness in stops." },
        { "contrast",   "Contrast",     0, 2, 1, "Color", "Pushes darks down and brights up around mid grey." },
        { "saturation", "Saturation",   0, 2, 1, "Color", "Color intensity. 0 = black and white." },
        { "temperature","Temperature", -1, 1, 0, "Color", "Cooler (blue) to warmer (orange) white balance." },
        { "tint",       "Tint",        -1, 1, 0, "Color", "Green to magenta balance." },
        { "fade",       "Fade",         0, 1, 0, "Color", "Lifts the blacks for a faded film print." },
        { "brightness", "Brightness",  -1, 1, 0, "Adjust", "Brighter or darker midtones; black and white points stay put." },
        { "highlights", "Highlights",  -1, 1, 0, "Adjust", "Pull back (-) or push (+) only the bright parts." },
        { "shadows",    "Shadows",     -1, 1, 0, "Adjust", "Open up (+) or deepen (-) only the dark parts." },
        { "sharpen",    "Sharpen",      0, 1, 0, "Adjust", "Crisper edges and fine detail." },
        { "crvM0", "Curve M 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvM1", "Curve M 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvM2", "Curve M 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvM3", "Curve M 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvM4", "Curve M 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "crvR0", "Curve R 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvR1", "Curve R 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvR2", "Curve R 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvR3", "Curve R 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvR4", "Curve R 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "crvG0", "Curve G 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvG1", "Curve G 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvG2", "Curve G 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvG3", "Curve G 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvG4", "Curve G 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "crvB0", "Curve B 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvB1", "Curve B 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvB2", "Curve B 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvB3", "Curve B 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvB4", "Curve B 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "splitTone",  "Teal/Orange",  0, 1, 0, "Color", "Teal shadows and warm highlights." },
        { "crush",      "Crush Blacks", 0, 1, 0, "Color", "Deep, inky shadows (Gothic)." },
        { "lutMix",     "LUT Intensity",0, 1, 1, "Color", "How much of the loaded .cube LUT is applied." },
        { "grain",      "Film Grain",   0, 1, 0, "Film",  "Procedural, animated film grain. Never a looped overlay." },
        { "grainSize",  "Grain Gauge",  0, 2, 1, "Film",  "0 = 35mm fine, 1 = 16mm, 2 = 8mm coarse." },
        { "halation",   "Halation",     0, 1, 0, "Film",  "Red glow bleeding around bright highlights." },
        { "bloom",      "Bloom",        0, 1, 0, "Film",  "Soft glow from the brightest parts of the image." },
        { "vignette",   "Vignette",     0, 1, 0, "Film",  "Darkens the frame edges." },
        { "letterbox",  "Letterbox",    0, 1, 0, "Film",  "Cinema bars (0.5 = 2.39:1)." },
        { "letterboxRatio", "Bars ratio", 1.33f, 2.76f, 2.39f, "Film", "Movie border aspect: 2.39 scope, 2.00, 1.85 flat." },
        { "crt",        "CRT Screen",   0, 1, 0, "Distort","Real tube TV: RGB phosphor mask, scanlines, convergence offset, curved glass and glow." },
        { "vhs",        "VHS",          0, 1, 0, "Distort","Tape chroma bleed, tracking wobble, noise lines." },
        { "rgbSplit",   "RGB Split",    0, 1, 0, "Distort","Separates red and blue channels horizontally." },
        { "glitch",     "Glitch",       0, 1, 0, "Distort","Block displacement bursts, driven by the beat." },
        { "kickZoom",   "Kick Zoom",    0, 1, 0, "Beat",  "Punch-in on every detected kick." },
        { "snareFlash", "Snare Flash",  0, 1, 0, "Beat",  "White flash on every detected snare." },
        { "flashWhite", "White Flash",  0, 1, 0, "Beat",  "Simple white flash on every kick. Amount = how bright." },
        { "flashBlack", "Black Flash",  0, 1, 0, "Beat",  "Simple black flash on every kick. Amount = how dark." },
        { "blackFlash2","Black Flash 2",0, 1, 0, "Beat",  "CapCut-style Black Flash 2: black/white shutter strobe on snares with a ripple and warp punch." },
        { "bf2Warp",    "Black Flash 2 Warp",0, 1, .5f, "Beat", "Strength of the ripple + warp that rides with the Black Flash 2 strobe." },
        { "xfRipple",   "Transformer: Acid Ripple", 0, 1, 0, "Transformer", "Rings rippling out from the centre of the frame." },
        { "xfWarble",   "Transformer: Warble", 0, 1, 0, "Transformer", "Fast wobbly heat-haze warble." },
        { "xfTwist",    "Transformer: Twist", -1, 1, 0, "Transformer", "Swirls the middle of the frame (negative = other way)." },
        { "xfLens",     "Transformer: Lens", -1, 1, 0, "Transformer", "Fisheye bulge (+) or pinch (-) of the whole frame." },
        { "shrinkGrow", "Shrink & Grow",0, 1, 0, "Motion",  "Smoothly grows then shrinks back across the effect layer. Amount = intensity." },
        { "zoomIn",     "Zoom In",    0, 1, 0, "Motion", "Smooth push-in across the effect layer. Amount = how far." },
        { "zoomOut",    "Zoom Out",   0, 1, 0, "Motion", "Smooth pull-out across the effect layer. Amount = how far." },
        { "handheld",   "Handheld",   0, 1, 0, "Motion", "Adds a bit of handheld camera drift to everything under the layer." },
        { "shake",      "Kick Shake",   0, 1, 0, "Beat",  "Camera shake triggered by kicks." },
        { "hatStrobe",  "Hi-Hat Strobe",0, 1, 0, "Beat",  "Exposure flicker on detected hi-hats." },
        { "beatGlitch", "Beat Glitch",  0, 1, 0, "Beat",  "Glitch + RGB split hits on snares." },
        { "invertHits", "Negative Flips",0, 1, 0, "Acid",  "Flips the frame to a negative for 1-2 frames on snare hits." },
        { "solarize",   "Solarize",     0, 1, 0, "Acid",  "Inverts the brightest tones like a darkroom solarization." },
        { "hueCycle",   "Hue Cycle",    0, 1, 0, "Acid",  "Rotates all colors over time and jumps on every kick." },
        { "posterize",  "Posterize",    0, 1, 0, "Acid",  "Reduces the image to flat color bands." },
        { "thermal",    "Thermal",      0, 1, 0, "Acid",  "Maps brightness to a heat-camera palette." },
        { "edgeGlow",   "Edge Glow",    0, 1, 0, "Acid",  "Neon glowing outlines over a darkened image." },
        { "kaleido",    "Kaleidoscope", 0, 1, 0, "Acid",  "Mirrors the frame into rotating segments." },
        { "liquid",     "Liquid Warp",  0, 1, 0, "Acid",  "Flowing wave displacement, pushed harder on kicks." },
        { "trails",     "Echo Trails",  0, 1, 0, "Acid",  "Frame echo: bright motion leaves fading trails." },
        { "lightLeak",  "Light Leak",   0, 1, 0, "Overlay","Generated warm light leaks drifting across the frame." },
        { "filmBurn",   "Film Burn",    0, 1, 0, "Overlay","Orange-white burn flare on every cut." },
        { "dust",       "Dust & Scratches",0,1,0, "Overlay","Generated dust specks, hairs and vertical scratches." },
        { "flare",      "Anamorphic Flare",0,1,0, "Overlay","Blue horizontal streaks from bright highlights." },
    };
    return d;
}

const std::vector<ParamDef>& clipParams()
{
    static const std::vector<ParamDef> d {
        { "exposure",   "Exposure",   -2, 2, 0, "Clip", "Brightness of this clip only." },
        { "contrast",   "Contrast",    0, 2, 1, "Clip", "Contrast of this clip only." },
        { "saturation", "Saturation",  0, 2, 1, "Clip", "Saturation of this clip only." },
        { "brightness", "Brightness", -1, 1, 0, "Adjust", "Brighter or darker midtones of this clip." },
        { "highlights", "Highlights", -1, 1, 0, "Adjust", "Bright parts of this clip only." },
        { "shadows",    "Shadows",    -1, 1, 0, "Adjust", "Dark parts of this clip only." },
        { "temperature","Temperature",-1, 1, 0, "Adjust", "Cooler or warmer white balance for this clip." },
        { "tint",       "Tint",       -1, 1, 0, "Adjust", "Green to magenta for this clip." },
        { "crvM0", "Curve M 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvM1", "Curve M 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvM2", "Curve M 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvM3", "Curve M 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvM4", "Curve M 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "crvR0", "Curve R 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvR1", "Curve R 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvR2", "Curve R 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvR3", "Curve R 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvR4", "Curve R 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "crvG0", "Curve G 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvG1", "Curve G 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvG2", "Curve G 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvG3", "Curve G 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvG4", "Curve G 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "crvB0", "Curve B 0", 0, 1, 0.00f, "Curves", "Tone curve point." },
        { "crvB1", "Curve B 1", 0, 1, 0.25f, "Curves", "Tone curve point." },
        { "crvB2", "Curve B 2", 0, 1, 0.50f, "Curves", "Tone curve point." },
        { "crvB3", "Curve B 3", 0, 1, 0.75f, "Curves", "Tone curve point." },
        { "crvB4", "Curve B 4", 0, 1, 1.00f, "Curves", "Tone curve point." },
        { "scale",      "Scale",     0.0f, 10, 1, "Transform", "Uniform scale of this clip, 0-1000%." },
        { "scaleX",     "Scale X",   0.01f, 10, 1, "Transform", "Width multiplier (non-uniform scale)." },
        { "scaleY",     "Scale Y",   0.01f, 10, 1, "Transform", "Height multiplier (non-uniform scale)." },
        { "cropL",      "Crop Left",   0, 1, 0, "Transform", "Crop from the left edge of the source." },
        { "cropR",      "Crop Right",  0, 1, 0, "Transform", "Crop from the right edge of the source." },
        { "cropT",      "Crop Top",    0, 1, 0, "Transform", "Crop from the top edge of the source." },
        { "cropB",      "Crop Bottom", 0, 1, 0, "Transform", "Crop from the bottom edge of the source." },
        { "posX",       "Position X", -2, 2, 0, "Transform", "Horizontal offset (or drag the clip in the preview)." },
        { "posY",       "Position Y", -2, 2, 0, "Transform", "Vertical offset (or drag the clip in the preview)." },
        { "zoomIn",     "Zoom In",    0, 1, 0, "Motion", "Smooth push-in across the clip. Amount = how far." },
        { "zoomOut",    "Zoom Out",   0, 1, 0, "Motion", "Smooth pull-out across the clip. Amount = how far." },
        { "handheld",   "Handheld",   0, 1, 0, "Motion", "A bit of handheld camera drift on a tripod shot." },
        { "rotation",   "Rotation", -180, 180, 0, "Transform", "Rotation in degrees." },
        { "mirror",     "Mirror",      0, 1, 0, "Transform", "Flip horizontally (1 = on)." },
        { "reframe",    "Reframe",     0, 2, 0, "Transform", "0 = Fill (crop), 1 = Fit (bars), 2 = Blur Fill (fit over a blurred copy)." },
        { "blend",      "Blend Mode",  0, 11, 0, "Composite", "Normal, Add, Screen, Multiply, Overlay, Soft Light, Lighten, Darken, Difference, Color Dodge, Hard Light, Exclusion." },
        { "chromaKey",  "Chroma Key",  0, 1, 0, "Composite", "Keys out a color (green screen). Amount = key strength." },
        { "keyHue",     "Key Hue",     0, 360, 120, "Composite", "Color to key out, in degrees (120 = green, 240 = blue)." },
        { "keyTol",     "Key Tolerance",0, 1, .3f, "Composite", "How wide a range of the key color is removed." },
        { "lumaKey",    "Luma Key",    0, 1, 0, "Composite", "Keys out dark areas, for light leaks and fire filmed on black." },
        { "lumaThresh", "Luma Threshold",0, 1, .15f, "Composite", "Brightness below which the luma key removes pixels." },
        { "maskShape",  "Mask",        0, 2, 0, "Mask", "0 = none, 1 = ellipse, 2 = rectangle." },
        { "maskW",      "Mask Width",  0, 1.5f, .6f, "Mask", "Mask width relative to the frame." },
        { "maskH",      "Mask Height", 0, 1.5f, .6f, "Mask", "Mask height relative to the frame." },
        { "maskFeather","Mask Feather",0, 1, .3f, "Mask", "Soft edge of the mask." },
        { "maskInvert", "Invert Mask", 0, 1, 0, "Mask", "Show outside the mask instead of inside." },
        { "opacity",    "Opacity",     0, 1, 1, "Composite", "Keyframeable opacity of this clip." },
        { "blur",       "Blur",        0, 1, 0, "Stylize", "Gaussian-style blur of this clip (Premiere Gaussian Blur)." },
        { "lensCurve",  "Lens Distortion", -1, 1, 0, "Stylize", "Barrel (negative) or pincushion (positive) lens curvature." },
        { "xfRipple",   "Transformer: Acid Ripple", 0, 1, 0, "Transformer", "Rings rippling out from the centre of this clip." },
        { "xfWarble",   "Transformer: Warble", 0, 1, 0, "Transformer", "Fast wobbly heat-haze warble on this clip." },
        { "xfTwist",    "Transformer: Twist", -1, 1, 0, "Transformer", "Swirls the middle of this clip (negative = other way)." },
        { "posterizeTime", "Posterize Time", 0, 60, 0, "Stylize", "Plays this clip at a lower frame rate (0 = off)." },
        { "invert",     "Invert",      0, 1, 0, "Stylize", "Inverts the colors of this clip." },
        { "textSize",   "Text Size",   0.02f, 0.5f, 0.12f, "Text", "Letter height relative to the frame." },
        { "textR",      "Text Red",    0, 1, 1, "Text", "Text color: red." },
        { "textG",      "Text Green",  0, 1, 1, "Text", "Text color: green." },
        { "textB",      "Text Blue",   0, 1, 1, "Text", "Text color: blue." },
        { "textBold",   "Bold",        0, 1, 1, "Text", "Bold weight." },
        { "textOutline","Outline",     0, 1, 0, "Text", "Black outline around the letters." },
        { "textReveal", "Reveal",      0, 1, 1, "Text", "Share of the letters shown (keyframe it for a typewriter)." },
    };
    return d;
}

const ParamDef* findParam (const std::vector<ParamDef>& defs, const juce::String& id)
{
    for (auto& d : defs) if (id == d.id) return &d;
    return nullptr;
}

juce::File contentDir()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    for (auto d : { dir.getSiblingFile ("Resources").getChildFile ("Content"), dir.getChildFile ("Content") })
        if (d.isDirectory()) return d;
    return {};
}

const std::vector<Look>& lookLibrary()
{
    static const std::vector<Look> L {
        { "Clean Rec709", "Color", "Neutral, no grade.", {} },
        { "35mm Fine", "Film Grain", "Fine 35mm print grain, barely-there halation.",
          { { "grain", .35f }, { "grainSize", .6f }, { "halation", .1f }, { "fade", .05f } } },
        { "35mm Vision 500T", "Film Grain", "Tungsten 500T stock: visible grain, cool shadows, red halation on lights.",
          { { "grain", .5f }, { "grainSize", .8f }, { "halation", .4f }, { "temperature", -.12f }, { "contrast", 1.06f }, { "fade", .08f } } },
        { "16mm Heavy", "Film Grain", "Chunky 16mm grain with soft vignette.",
          { { "grain", .75f }, { "grainSize", 1.3f }, { "halation", .25f }, { "vignette", .35f }, { "fade", .12f } } },
        { "Super 8 Coarse", "Film Grain", "Big Super 8 grain, warm, lifted blacks.",
          { { "grain", .95f }, { "grainSize", 2.2f }, { "temperature", .25f }, { "fade", .22f }, { "vignette", .5f }, { "saturation", .85f } } },
        { "B&W Silver Halide", "Film Grain", "Black-and-white Tri-X look: deep blacks, crisp silver grain.",
          { { "saturation", 0.0f }, { "grain", .65f }, { "grainSize", .9f }, { "contrast", 1.3f }, { "crush", .25f }, { "vignette", .3f } } },
        { "Push-Processed Grit", "Film Grain", "Pushed two stops: harsh grain, crushed shadows, punchy contrast.",
          { { "grain", .9f }, { "grainSize", 1.1f }, { "contrast", 1.25f }, { "crush", .35f }, { "saturation", .8f }, { "halation", .2f } } },
        { "Grain Only", "Film Grain", "Just grain, no color change. Stack it on any look.",
          { { "grain", .5f }, { "grainSize", 1.0f } } },
        { "Teal Night", "Cinematic", "Teal shadows, warm skin, soft bloom.",
          { { "splitTone", .7f }, { "contrast", 1.15f }, { "saturation", .9f }, { "bloom", .25f }, { "vignette", .35f }, { "letterbox", .5f } } },
        { "Warm Golden Hour", "Cinematic", "Warm highlights and gentle halation.",
          { { "temperature", .35f }, { "halation", .35f }, { "bloom", .2f }, { "contrast", 1.05f }, { "grain", .2f } } },
        { "Faded Print", "Film", "Lifted blacks, muted color, fine grain.",
          { { "fade", .35f }, { "saturation", .75f }, { "contrast", .9f }, { "grain", .35f }, { "grainSize", .3f } } },
        { "16mm Documentary", "Film", "16mm-style grain, halation and vignette.",
          { { "grain", .55f }, { "grainSize", 1.0f }, { "halation", .3f }, { "vignette", .4f }, { "fade", .12f } } },
        { "Super 8 Memory", "Film", "Coarse 8mm grain, warm, soft and vignetted.",
          { { "grain", .8f }, { "grainSize", 2.0f }, { "temperature", .3f }, { "vignette", .6f }, { "fade", .2f }, { "halation", .25f } } },
        { "Gothic Noir", "Gothic", "Deep blacks, desaturated cold tones.",
          { { "crush", .6f }, { "saturation", .35f }, { "temperature", -.3f }, { "contrast", 1.35f }, { "vignette", .55f }, { "grain", .3f } } },
        { "Blood Moon", "Gothic", "Crushed shadows with red halation.",
          { { "crush", .5f }, { "tint", .2f }, { "halation", .7f }, { "saturation", .7f }, { "contrast", 1.25f } } },
        { "VHS Rental", "VHS", "Tape bleed, noise and soft chroma.",
          { { "vhs", .75f }, { "rgbSplit", .25f }, { "saturation", 1.15f }, { "fade", .15f }, { "grain", .2f } } },
        { "Y2K Chrome", "Y2K", "Clean cool chrome: soft bloom, silver highlights, gentle halation.",
          { { "temperature", -.18f }, { "bloom", .35f }, { "halation", .12f }, { "saturation", 1.08f }, { "contrast", 1.06f }, { "highlights", -.12f }, { "grain", .12f } } },
        { "Underground Grit", "Underground", "Dense film blacks, fine grain and a Black Flash 2 shutter on snares.",
          { { "contrast", 1.22f }, { "saturation", .72f }, { "crush", .3f }, { "grain", .35f }, { "grainSize", 1.15f }, { "vignette", .4f }, { "blackFlash2", .45f } } },
        { "Black Flash 2 (CapCut)", "Beat", "Black/white shutter strobe on every snare with the ripple + warp punch. Set Black Flash 2 Warp for warp strength.",
          { { "blackFlash2", .8f }, { "bf2Warp", .6f } } },
        { "White Flash", "Beat", "Flashes white on every kick. Set White Flash for strength.", { { "flashWhite", .8f } } },
        { "Black Flash", "Beat", "Flashes black on every kick. Set Black Flash for strength.", { { "flashBlack", .8f } } },
        { "Shrink & Grow", "Motion", "Smooth grow then shrink back over the layer. Set Shrink & Grow for intensity.", { { "shrinkGrow", .6f } } },
        { "Zoom In", "Motion", "Simple smooth push-in over the layer. Set Zoom In for how far.", { { "zoomIn", .5f } } },
        { "Zoom Out", "Motion", "Simple smooth pull-out over the layer. Set Zoom Out for how far.", { { "zoomOut", .5f } } },
        { "Handheld", "Motion", "CapCut-style handheld: a bit of camera drift on tripod shots.", { { "handheld", .5f } } },
        { "Acid Ripple Pool", "Acid", "Slow acid ripples with liquid warp and cycling hues.", { { "xfRipple", .6f }, { "liquid", .3f }, { "hueCycle", .3f } } },
        { "Acid Kaleido Melt", "Acid", "Kaleidoscope mirror melting through a liquid warp.", { { "kaleido", .5f }, { "liquid", .45f }, { "saturation", 1.4f } } },
        { "Acid Warble Haze", "Acid", "Heat-haze warble with bloom and a hue drift.", { { "xfWarble", .6f }, { "bloom", .35f }, { "hueCycle", .2f } } },
        { "Acid Twist Tunnel", "Acid", "Swirl into a fisheye bulge.", { { "xfTwist", .6f }, { "xfLens", .4f }, { "saturation", 1.3f } } },
        { "Acid Kaleido Ripple", "Acid", "Kaleidoscope with rippling rings.", { { "kaleido", .4f }, { "xfRipple", .45f }, { "bloom", .25f } } },
        { "Acid Warble Kaleido", "Acid", "Kaleidoscope with a wobbly warble and color cycle.", { { "kaleido", .35f }, { "xfWarble", .5f }, { "hueCycle", .4f } } },
        { "Acid Liquid Pinch", "Acid", "Liquid warp pinched toward the centre.", { { "liquid", .5f }, { "xfLens", -.45f }, { "saturation", 1.35f } } },
        { "Acid Full Trip", "Acid", "Everything: kaleidoscope, ripple, warble, twist and hues.", { { "kaleido", .3f }, { "xfRipple", .35f }, { "xfWarble", .3f }, { "xfTwist", .3f }, { "hueCycle", .45f }, { "bloom", .3f } } },
        { "Transformer: Acid Ripple", "Transformer", "Rippling rings over everything under this layer. Strength in the Inspector.", { { "xfRipple", .5f } } },
        { "Transformer: Warble", "Transformer", "Wobbly warble over everything under this layer.", { { "xfWarble", .5f } } },
        { "Transformer: Twist", "Transformer", "Swirl warp over everything under this layer.", { { "xfTwist", .5f } } },
        { "Transformer: Lens Bulge", "Transformer", "Fisheye bulge over everything under this layer (negative pinches).", { { "xfLens", .5f } } },
        { "Acid Abstract", "Abstract", "Acid Trip style: kaleidoscope mirror, liquid warp and cycling hues.",
          { { "saturation", 1.6f }, { "kaleido", .35f }, { "liquid", .4f }, { "hueCycle", .4f }, { "bloom", .35f } } },
        { "CRT TV", "VHS", "Curved tube TV: phosphor mask, scanlines and a soft glow.",
          { { "crt", .8f }, { "bloom", .2f }, { "contrast", 1.08f }, { "saturation", 1.1f }, { "vignette", .3f } } },
        { "VHS Camcorder", "VHS", "Home-video tape: chroma bleed, tracking wobble, warm faded blacks.",
          { { "vhs", .6f }, { "fade", .12f }, { "temperature", .08f }, { "saturation", .9f }, { "grain", .15f } } },
        { "VHS on CRT", "VHS", "Tape played back on a tube TV: bleed, wobble, phosphor mask and glow.",
          { { "vhs", .55f }, { "crt", .7f }, { "fade", .1f }, { "bloom", .15f }, { "vignette", .25f } } },
        { "Performance Punch", "Beat", "Kick zoom, snare flash and kick shake.",
          { { "kickZoom", .6f }, { "snareFlash", .45f }, { "shake", .35f }, { "contrast", 1.1f } } },
        { "Mono Contrast", "Color", "High contrast black and white.",
          { { "saturation", 0.0f }, { "contrast", 1.4f }, { "grain", .3f } } },
        // tuned against the quality-bar references (docs/REFERENCES.md)
        { "Night Teal Flip", "Reference", "Night-street teal cast, low saturation, push-ins, white flash bursts and negative flips on hits.",
          { { "temperature", -.35f }, { "tint", -.08f }, { "saturation", .7f }, { "contrast", 1.2f }, { "crush", .2f }, { "splitTone", .35f },
            { "grain", .3f }, { "vignette", .35f }, { "kickZoom", .35f }, { "snareFlash", .35f }, { "invertHits", .45f }, { "bloom", .15f } } },
        { "Warm Crush", "Reference", "Crushed inky blacks, warm brick reds, heavy vignette, clean hard cuts.",
          { { "temperature", .3f }, { "tint", .1f }, { "saturation", .65f }, { "contrast", 1.3f }, { "crush", .55f }, { "vignette", .7f },
            { "halation", .35f }, { "grain", .35f }, { "grainSize", 1.0f } } },
        { "Sky Chrome Haze", "Reference", "Bright sky blues and gold, chrome bloom, light leaks and fast smeared cuts.",
          { { "exposure", .2f }, { "saturation", 1.2f }, { "contrast", 1.05f }, { "splitTone", .3f }, { "bloom", .5f }, { "halation", .2f },
            { "lightLeak", .45f }, { "flare", .3f }, { "trails", .2f }, { "grain", .15f } } },
        { "Acid Cutout", "Acid", "Thermal edge-glow trip with hue cycling and echo trails.",
          { { "thermal", .5f }, { "edgeGlow", .5f }, { "hueCycle", .4f }, { "trails", .5f }, { "liquid", .25f }, { "contrast", 1.15f } } },
        { "Kaleido Strobe", "Acid", "Kaleidoscope mirror, posterized color and hi-hat strobing.",
          { { "kaleido", .8f }, { "posterize", .4f }, { "hueCycle", .3f }, { "hatStrobe", .3f }, { "saturation", 1.4f } } },
        { "Burnt Film", "Film", "16mm with dust, scratches and film burns on every cut.",
          { { "grain", .5f }, { "grainSize", 1.0f }, { "dust", .55f }, { "filmBurn", .6f }, { "fade", .15f }, { "temperature", .15f }, { "vignette", .4f } } },
    };
    return L;
}

void applyLook (FxStack& s, const Look& look, float amount)
{
    for (auto& [id, v] : look.p)
    {
        auto* d = findParam (sequenceParams(), id);
        float def = d ? d->def : 0.0f;
        s.set (id, def + (v - def) * amount);
    }
}

juce::StringArray styleModes() { return { "Classic", "Gothic", "VHS", "Y2K", "Underground", "Abstract" }; }

juce::String styleModeHelp (const juce::String& m)
{
    if (m == "Classic") return "Clean cinematic grade, letterbox, gentle film texture.";
    if (m == "Gothic") return "Crushed blacks, cold desaturation, red halation.";
    if (m == "VHS") return "Tape bleed, chroma shift, noise and warm fade.";
    if (m == "Y2K") return "Glossy chrome bloom, cool highlights, saturated color.";
    if (m == "Underground") return "Harsh contrast, heavy grain, beat glitches.";
    if (m == "Abstract") return "Split channels, strobes and saturated color.";
    return {};
}

void applyStyleMode (FxStack& s, const juce::String& mode)
{
    static const std::map<juce::String, juce::String> map {
        { "Classic", "Teal Night" }, { "Gothic", "Gothic Noir" }, { "VHS", "VHS Rental" },
        { "Y2K", "Y2K Chrome" }, { "Underground", "Underground Grit" }, { "Abstract", "Acid Abstract" } };
    auto it = map.find (mode);
    if (it == map.end()) return;
    // keep beat-reactive settings, reset the grade
    std::map<juce::String, float> keep;
    for (auto id : { "kickZoom", "snareFlash", "shake", "hatStrobe" }) if (s.p.count (id)) keep[id] = s.p[id];
    s.p = keep;
    for (auto& l : lookLibrary()) if (l.name == it->second) applyLook (s, l, 1.0f);
}

juce::StringArray transitionNames() { return { "Cut", "Dissolve", "Dip Black", "Dip White", "White Flash", "Zoom Punch", "Whip Pan", "Glitch",
                                               "Spin", "RGB Split", "Blur", "Luma Wipe", "Ink Burn", "Shake", "Stretch", "Mirror", "Prism", "Light Leak", "Black Flash" }; }

bool Lut3D::load (const juce::File& f, juce::String& error)
{
    juce::StringArray lines; lines.addLines (f.loadFileAsString());
    size = 0; rgb.clear();
    float dmin[3] { 0, 0, 0 }, dmax[3] { 1, 1, 1 };
    for (auto& raw : lines)
    {
        auto l = raw.trim();
        if (l.isEmpty() || l.startsWithChar ('#')) continue;
        if (l.startsWith ("TITLE")) { title = l.fromFirstOccurrenceOf ("\"", false, false).upToLastOccurrenceOf ("\"", false, false); continue; }
        if (l.startsWith ("LUT_3D_SIZE")) { size = l.fromFirstOccurrenceOf (" ", false, false).trim().getIntValue(); continue; }
        if (l.startsWith ("LUT_1D_SIZE")) { error = "1D LUTs are not supported, use a 3D .cube"; return false; }
        if (l.startsWith ("DOMAIN_MIN") || l.startsWith ("DOMAIN_MAX"))
        {
            auto t = juce::StringArray::fromTokens (l, " \t", ""); t.removeEmptyStrings();
            float* dst = l.startsWith ("DOMAIN_MIN") ? dmin : dmax;
            for (int i = 0; i < 3 && i + 1 < t.size(); ++i) dst[i] = t[i + 1].getFloatValue();
            continue;
        }
        if (! (juce::CharacterFunctions::isDigit (l[0]) || l[0] == '-' || l[0] == '.')) continue;
        auto t = juce::StringArray::fromTokens (l, " \t", ""); t.removeEmptyStrings();
        if (t.size() < 3) continue;
        for (int i = 0; i < 3; ++i) rgb.push_back ((t[i].getFloatValue() - dmin[i]) / std::max (1e-6f, dmax[i] - dmin[i]));
    }
    if (size < 2 || size > 128) { error = "Missing or invalid LUT_3D_SIZE"; return false; }
    if (rgb.size() != (size_t) size * size * size * 3) { error = "LUT has " + juce::String ((int) rgb.size() / 3) + " entries, expected " + juce::String (size * size * size); return false; }
    if (title.isEmpty()) title = f.getFileNameWithoutExtension();
    return true;
}
}
