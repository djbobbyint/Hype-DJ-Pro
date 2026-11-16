#include "WaveformViews.h"

namespace hype::ui
{
juce::Colour bandColour (uint8_t low, uint8_t mid, uint8_t high, float brightness)
{
    // Square the band energies so the dominant band sets the hue (more contrast than a plain mix):
    // bass -> red/magenta, mids -> green, highs -> cyan/blue.
    const float l = juce::square (low / 255.0f), m = juce::square (mid / 255.0f), h = juce::square (high / 255.0f);
    const float sum = l + m + h + 1.0e-6f;
    float r  = (l * 1.00f + m * 0.15f + h * 0.10f) / sum;
    float gr = (l * 0.10f + m * 0.90f + h * 0.55f) / sum;
    float b  = (l * 0.35f + m * 0.15f + h * 1.00f) / sum;
    const float mx = juce::jmax (r, gr, b, 1.0e-4f);
    r /= mx; gr /= mx; b /= mx;
    return juce::Colour::fromFloatRGBA (r * brightness, gr * brightness, b * brightness, 1.0f);
}

//==============================================================================
ScrollingWaveform::ScrollingWaveform (AudioEngine& e, int d) : engine (e), deck (d)
{
    setOpaque (true);
}

void ScrollingWaveform::update (const Deck::State& s, const LoadedTrack::Ptr& t)
{
    state = s;
    track = t;
    repaint();
}

void ScrollingWaveform::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const auto accent = Palette::forDeck (deck);
    g.fillAll (Palette::well);

    const float w = (float) bounds.getWidth(), h = (float) bounds.getHeight();
    const float centreY = h * 0.5f;
    const float playheadX = w * 0.5f;

    auto waveform = track != nullptr ? track->getWaveform() : nullptr;

    if (waveform == nullptr || ! state.loaded)
    {
        g.setColour (Palette::outline);
        g.drawHorizontalLine ((int) centreY, 0.0f, w);
        g.setColour (Palette::textFaint);
        g.setFont (HypeLookAndFeel::font (14.0f, true));
        g.drawText (juce::String ("DECK ") + (deck == 0 ? "A" : "B") + "  -  drop a track here or press LOAD",
                    bounds, juce::Justification::centred);
    }
    else
    {
        const double secondsPerPixel = visibleSeconds / w;
        const double startTime = state.elapsedSeconds - playheadX * secondsPerPixel;
        const double binsPerSecond = waveform->binsPerSecond();
        const int numBins = waveform->getNumBins();

        // Beat grid (behind the waveform); every 4th beat from the grid anchor is a bar line.
        if (state.originalBpm > 0.0)
        {
            const double beatLen = 60.0 / state.originalBpm;
            const auto firstK = (long) std::floor ((startTime - state.firstBeatSeconds) / beatLen);
            const auto lastK  = (long) std::ceil ((startTime + visibleSeconds - state.firstBeatSeconds) / beatLen);
            for (long k = firstK; k <= lastK; ++k)
            {
                const double t = state.firstBeatSeconds + k * beatLen;
                if (t < 0.0 || t > state.durationSeconds)
                    continue;
                const float x = (float) ((t - startTime) / secondsPerPixel);
                const bool bar = ((k % 4) + 4) % 4 == 0;
                g.setColour (bar ? Palette::textDim.withAlpha (0.55f) : Palette::outline);
                g.fillRect (x - (bar ? 0.75f : 0.5f), 0.0f, bar ? 1.5f : 1.0f, h);
            }
        }

        // Waveform columns
        for (int x = 0; x < bounds.getWidth(); ++x)
        {
            const double t0 = startTime + x * secondsPerPixel;
            const double t1 = t0 + secondsPerPixel;
            int b0 = (int) std::floor (t0 * binsPerSecond);
            int b1 = juce::jmax (b0 + 1, (int) std::ceil (t1 * binsPerSecond));
            if (b1 <= 0 || b0 >= numBins)
                continue;
            b0 = juce::jmax (0, b0);
            b1 = juce::jmin (numBins, b1);

            uint8_t pk = 0;
            int lo = 0, mi = 0, hi = 0;
            for (int b = b0; b < b1; ++b)
            {
                pk = juce::jmax (pk, waveform->peak[(size_t) b]);
                lo += waveform->low[(size_t) b];
                mi += waveform->mid[(size_t) b];
                hi += waveform->high[(size_t) b];
            }
            const int n = b1 - b0;
            const float amp = (pk / 255.0f) * (centreY - 2.0f);
            const bool past = x < (int) playheadX;
            g.setColour (bandColour ((uint8_t) (lo / n), (uint8_t) (mi / n), (uint8_t) (hi / n), past ? 0.55f : 1.0f));
            g.fillRect ((float) x, centreY - amp, 1.0f, amp * 2.0f);
        }

        // Cue marker
        const float cueX = (float) ((state.cueSeconds - startTime) / secondsPerPixel);
        if (cueX >= -6.0f && cueX <= w + 6.0f)
        {
            g.setColour (Palette::cue);
            g.fillRect (cueX - 0.5f, 0.0f, 1.0f, h);
            juce::Path tri;
            tri.addTriangle (cueX - 6.0f, 0.0f, cueX + 6.0f, 0.0f, cueX, 8.0f);
            g.fillPath (tri);
        }

        // Track end
        const float endX = (float) ((state.durationSeconds - startTime) / secondsPerPixel);
        if (endX < w)
        {
            g.setColour (Palette::background.withAlpha (0.7f));
            g.fillRect (juce::jmax (0.0f, endX), 0.0f, w - juce::jmax (0.0f, endX), h);
        }
    }

    // Playhead
    g.setColour (accent);
    g.fillRect (playheadX - 1.0f, 0.0f, 2.0f, h);
    g.setColour (accent.withAlpha (0.25f));
    g.fillRect (playheadX - 4.0f, 0.0f, 8.0f, h);

    // Deck badge
    auto badge = juce::Rectangle<int> (6, 6, 22, 18);
    g.setColour (accent);
    g.fillRoundedRectangle (badge.toFloat(), 3.0f);
    g.setColour (Palette::background);
    g.setFont (HypeLookAndFeel::font (13.0f, true));
    g.drawText (deck == 0 ? "A" : "B", badge, juce::Justification::centred);

    g.setColour (Palette::outline);
    g.drawRect (bounds);
}

void ScrollingWaveform::mouseDown (const juce::MouseEvent& e)
{
    lastDragX = e.x;
}

void ScrollingWaveform::mouseDrag (const juce::MouseEvent& e)
{
    if (! state.loaded)
        return;
    const int dx = e.x - lastDragX;
    lastDragX = e.x;
    const double secondsPerPixel = visibleSeconds / juce::jmax (1, getWidth());

    auto& d = engine.getDeck (deck);
    if (state.playing)
        d.setPitchBend (juce::jlimit (-0.2, 0.2, -dx * 0.01));   // drag left = speed up (pull the record)
    else if (dx != 0)
        d.seekRelativeSeconds (-dx * secondsPerPixel);
}

void ScrollingWaveform::mouseUp (const juce::MouseEvent&)
{
    if (state.playing)
        engine.getDeck (deck).setPitchBend (0.0);
}

void ScrollingWaveform::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    visibleSeconds = juce::jlimit (2.0, 32.0, visibleSeconds * (wheel.deltaY > 0 ? 0.8 : 1.25));
    repaint();
}

//==============================================================================
OverviewWaveform::OverviewWaveform (AudioEngine& e, int d) : engine (e), deck (d)
{
    setTooltip ("Click or drag to seek");
}

void OverviewWaveform::update (const Deck::State& s, const LoadedTrack::Ptr& t)
{
    if (t.get() != track.get())
        cacheValid = false;
    state = s;
    track = t;
    repaint();
}

void OverviewWaveform::rebuildCache()
{
    cacheValid = true;
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    cache = juce::Image (juce::Image::ARGB, getWidth(), getHeight(), true);
    juce::Graphics g (cache);
    auto waveform = track != nullptr ? track->getWaveform() : nullptr;
    if (waveform == nullptr || waveform->getNumBins() == 0)
        return;

    const int numBins = waveform->getNumBins();
    const float centreY = getHeight() * 0.5f;
    for (int x = 0; x < getWidth(); ++x)
    {
        const int b0 = (int) ((int64_t) x * numBins / getWidth());
        const int b1 = juce::jmax (b0 + 1, (int) ((int64_t) (x + 1) * numBins / getWidth()));
        uint8_t pk = 0;
        int lo = 0, mi = 0, hi = 0;
        for (int b = b0; b < b1 && b < numBins; ++b)
        {
            pk = juce::jmax (pk, waveform->peak[(size_t) b]);
            lo += waveform->low[(size_t) b];
            mi += waveform->mid[(size_t) b];
            hi += waveform->high[(size_t) b];
        }
        const int n = juce::jmax (1, juce::jmin (b1, numBins) - b0);
        const float amp = (pk / 255.0f) * (centreY - 1.0f);
        g.setColour (bandColour ((uint8_t) (lo / n), (uint8_t) (mi / n), (uint8_t) (hi / n)));
        g.fillRect ((float) x, centreY - amp, 1.0f, amp * 2.0f);
    }
}

void OverviewWaveform::paint (juce::Graphics& g)
{
    if (! cacheValid)
        rebuildCache();

    const auto r = getLocalBounds().toFloat();
    g.setColour (Palette::well);
    g.fillRect (r);

    if (! state.loaded || track == nullptr)
    {
        g.setColour (Palette::outline);
        g.drawHorizontalLine (getHeight() / 2, 0.0f, r.getWidth());
    }
    else
    {
        g.drawImageAt (cache, 0, 0);
        const float px = (float) (state.position * r.getWidth());
        g.setColour (Palette::background.withAlpha (0.55f));      // played part dimmed
        g.fillRect (0.0f, 0.0f, px, r.getHeight());

        if (state.durationSeconds > 0.0)
        {
            const float cueX = (float) (state.cueSeconds / state.durationSeconds * r.getWidth());
            g.setColour (Palette::cue);
            g.fillRect (cueX - 0.5f, 0.0f, 1.0f, r.getHeight());
        }

        // Last 30 s warning band
        if (state.remainingSeconds < 30.0 && state.playing)
        {
            g.setColour (Palette::danger.withAlpha ((juce::Time::getMillisecondCounter() / 400) % 2 ? 0.35f : 0.1f));
            g.fillRect (r.withTrimmedTop (r.getHeight() - 3.0f));
        }

        g.setColour (Palette::text);
        g.fillRect (px - 1.0f, 0.0f, 2.0f, r.getHeight());
    }

    g.setColour (Palette::outline);
    g.drawRect (r, 1.0f);
}

void OverviewWaveform::seekTo (int x)
{
    if (state.loaded && getWidth() > 0)
        engine.getDeck (deck).seekToNormalised (juce::jlimit (0.0, 1.0, (double) x / getWidth()));
}

void OverviewWaveform::mouseDown (const juce::MouseEvent& e) { seekTo (e.x); }
void OverviewWaveform::mouseDrag (const juce::MouseEvent& e) { seekTo (e.x); }
} // namespace hype::ui
