#pragma once

#include <cstdint>

namespace hype
{
/** Global engine constants. Anything here is compile-time and shared by all decks. */
namespace config
{
    constexpr int    numDecks              = 2;

    /** The device callback is split into chunks of at most this many frames, so every
        internal buffer can be pre-allocated regardless of the device's buffer size. */
    constexpr int    maxInternalBlock      = 512;

    /** Length of the de-click crossfade used for play/pause/seek/load/mode switches. */
    constexpr double declickSeconds        = 0.004;

    /** Smoothing times for continuous parameters (avoids zipper noise). */
    constexpr double speedSmoothingSeconds = 0.030;
    constexpr double gainSmoothingSeconds  = 0.020;

    constexpr double minSpeed              = 0.0;   // vinyl mode can go all the way to a stop
    constexpr double maxSpeed              = 4.0;
    constexpr double minStretchRate        = 0.25;  // key-lock path is clamped to this
    constexpr double maxPitchSemitones     = 24.0;
    constexpr float  minTrimDb             = -24.0f;
    constexpr float  maxTrimDb             = 12.0f;

    /** Output chunk used when driving the time-stretcher, so speed automation is followed closely. */
    constexpr int    stretchChunk          = 64;

    /** Tracks longer than this are rejected (decoded audio is held in RAM as 32-bit float). */
    constexpr double maxTrackSeconds       = 60.0 * 60.0;

    /** Capacity of the lock-free command FIFOs (UI -> audio thread). */
    constexpr int    commandQueueSize      = 256;
    constexpr int    maxScheduledCommands  = 64;
} // namespace config
} // namespace hype
