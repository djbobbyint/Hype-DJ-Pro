#include "OutputRouter.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace hype
{
void OutputRouter::route (float* const* outputs, int numOut, int n,
                          const float* mL, const float* mR, const float* cL, const float* cR, bool split,
                          const float* const* dL, const float* const* dR) const noexcept
{
    auto add = [&] (int ch, const float* src)
    {
        if (ch >= 0 && ch < numOut && outputs[ch] != nullptr)
            juce::FloatVectorOperations::add (outputs[ch], src, n);
    };

    for (int d = 0; d < numDecks; ++d)
    {
        add (directL[d].load (std::memory_order_relaxed), dL[d]);
        add (directR[d].load (std::memory_order_relaxed), dR[d]);
    }

    const int ml = masterL.load (std::memory_order_relaxed), mr = masterR.load (std::memory_order_relaxed);
    const int hl = cueL.load (std::memory_order_relaxed),    hr = cueR.load (std::memory_order_relaxed);

    if (! (split && ml == hl && mr == hr))
    {
        add (ml, mL);
        add (mr, mR);
    }
    add (hl, cL);
    add (hr, cR);
}
} // namespace hype
