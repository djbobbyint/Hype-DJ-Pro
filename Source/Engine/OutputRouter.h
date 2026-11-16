#pragma once

#include "Api/HypeEngineApi.h"
#include <atomic>

namespace hype
{
/**
    OutputRouter: places the master, headphone and (optional) per-deck direct signals onto
    physical device output channels. Configured by AudioDeviceManager (or tests/CLI) from any
    thread via atomics; route() runs on the audio thread. Channel numbers are physical and
    0-based; -1 disables an output.
*/
class OutputRouter
{
public:
    void setMasterOutput (int left, int right)              { masterL.store (left); masterR.store (right); }
    void setCueOutput (int left, int right)                 { cueL.store (left); cueR.store (right); }
    void setDirectOutput (int deck, int left, int right)    { directL[deck].store (left); directR[deck].store (right); }

    int getMasterOutputLeft() const noexcept  { return masterL.load(); }
    int getMasterOutputRight() const noexcept { return masterR.load(); }
    int getCueOutputLeft() const noexcept     { return cueL.load(); }
    int getCueOutputRight() const noexcept    { return cueR.load(); }

    /** Audio thread. `outputs` is indexed by physical channel (entries may be null for inactive
        channels) and must already be cleared. If split cue shares the master pair (single stereo
        device), the split headphone signal replaces the master there. */
    void route (float* const* outputs, int numOutputs, int numSamples,
                const float* masterLeft, const float* masterRight,
                const float* cueLeft, const float* cueRight, bool splitCueActive,
                const float* const* directLeft, const float* const* directRight) const noexcept;

private:
    std::atomic<int> masterL { 0 }, masterR { 1 }, cueL { 2 }, cueR { 3 };
    std::atomic<int> directL[numDecks] { {-1}, {-1} }, directR[numDecks] { {-1}, {-1} };
};
} // namespace hype
