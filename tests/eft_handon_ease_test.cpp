// Portable arithmetic regression for the weight rate limiter (EftHandOnEase);
// not an engine/HUD build or game test.
#include "../src/xrGame/EftHandOn.h"
#include <cassert>
#include <limits>

static const float DT = 1.f / 30.f;

int main()
{
    // Raw rule of the real clips: 1 -> 0 in 5 frames (hand leaves the grip).
    const float fall[] = {1.f, .8f, .6f, .4f, .2f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    {
        EftHandOnEase e;
        unsigned frame = 100;
        float prev = 1.f, out = 0.f;
        float first_zero_time = -1.f;
        for (unsigned i = 0; i < 12; ++i, ++frame)
        {
            out = e.step(fall[i], DT, .3f, frame);
            // never below raw on the way down, never faster than 1/blend_time
            assert(out >= fall[i] - 1e-6f);
            assert(prev - out <= DT / .3f + 1e-6f);
            assert(out <= prev + 1e-6f); // monotone
            if (out <= 0.f && first_zero_time < 0.f) first_zero_time = i * DT;
            prev = out;
        }
        assert(first_zero_time >= .3f - DT - 1e-4f); // full swing takes about blend_time
        assert(out == 0.f);
    }
    // 0 -> 1 in 5 frames (hand reaches the grip): limited the same way, ends exactly at raw.
    {
        EftHandOnEase e;
        unsigned frame = 1;
        e.step(0.f, DT, .3f, frame++);
        float prev = 0.f, out = 0.f;
        const float rise[] = {0.f, .2f, .4f, .6f, .8f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
        for (unsigned i = 0; i < 12; ++i, ++frame)
        {
            out = e.step(rise[i], DT, .3f, frame);
            assert(out <= rise[i] + 1e-6f);
            assert(out - prev <= DT / .3f + 1e-6f);
            assert(out >= prev - 1e-6f);
            prev = out;
        }
        assert(out == 1.f);
    }
    // Slow raw motion is passed through untouched (steady state is Tarkov's rule).
    {
        EftHandOnEase e;
        unsigned frame = 1;
        for (unsigned i = 0; i <= 30; ++i, ++frame)
        {
            const float raw = 1.f - .01f * i; // 0.3/s, slower than 1/.25 per s
            assert(e.step(raw, DT, .25f, frame) == raw);
        }
    }
    // No history (layer not live last frame): starts at the raw weight, no ease-in from 0.
    {
        EftHandOnEase e;
        assert(e.step(1.f, DT, .25f, 5) == 1.f);
        assert(e.step(.6f, DT, .25f, 6) > .6f);
        e.reset(); // an early return in update_handon
        assert(e.step(.3f, DT, .25f, 7) == .3f);
    }
    // A gap of frames (layer not updated: script/offhand ownership) forgets the history too.
    {
        EftHandOnEase e;
        assert(e.step(1.f, DT, .25f, 10) == 1.f);
        assert(e.step(0.f, DT, .25f, 11) > 0.f);
        assert(e.step(0.f, DT, .25f, 40) == 0.f); // 29 frames later: starts at raw
    }
    // blend_time 0 = raw rule, bit for bit, including large jumps.
    {
        EftHandOnEase e;
        assert(e.step(1.f, DT, 0.f, 1) == 1.f);
        assert(e.step(0.f, DT, 0.f, 2) == 0.f);
        assert(e.step(1.f, DT, 0.f, 3) == 1.f);
        assert(e.step(.5f, DT, 0.f, 3) == .5f); // same frame, still raw
    }
    // Repeated call in the same frame does not advance the limiter twice.
    {
        EftHandOnEase e;
        e.step(1.f, DT, .3f, 10);
        const float a = e.step(0.f, DT, .3f, 11);
        const float b = e.step(0.f, DT, .3f, 11);
        assert(a == b && a > 0.f);
    }
    // Hostile input: never NaN, never out of [0,1], never moves without time.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        EftHandOnEase e;
        unsigned frame = 1;
        assert(e.step(.5f, DT, .3f, frame++) == .5f);
        assert(e.step(0.f, 0.f, .3f, frame++) == .5f);   // dt 0 (pause): holds
        assert(e.step(0.f, -1.f, .3f, frame++) == .5f);  // negative dt: holds
        assert(e.step(0.f, nan, .3f, frame++) == .5f);   // NaN dt: holds
        assert(e.step(0.f, inf, .3f, frame++) == .5f);   // inf dt: holds
        float v = e.step(nan, DT, .3f, frame++);          // NaN raw -> raw 0
        assert(v >= 0.f && v <= .5f);
        v = e.step(7.f, DT, .3f, frame++);                // raw clamped to 1
        assert(v <= 1.f);
        v = e.step(.2f, DT, nan, frame++);                // invalid blend_time -> raw
        assert(v == .2f);
        v = e.step(.9f, DT, -1.f, frame++);
        assert(v == .9f);
    }
    // Profile value validation.
    assert(eft_handon_blend_time_valid(0.f));
    assert(eft_handon_blend_time_valid(EFT_HANDON_BLEND_TIME_DEFAULT));
    assert(eft_handon_blend_time_valid(EFT_HANDON_BLEND_TIME_MAX));
    assert(!eft_handon_blend_time_valid(-.1f));
    assert(!eft_handon_blend_time_valid(EFT_HANDON_BLEND_TIME_MAX + .01f));
    assert(!eft_handon_blend_time_valid(std::numeric_limits<float>::quiet_NaN()));
    assert(!eft_handon_blend_time_valid(std::numeric_limits<float>::infinity()));
    return 0;
}
