// Portable arithmetic regression; not an engine/HUD build or game test.
#include "../src/xrGame/EftHandOn.h"
#include <cassert>
#include <limits>
int main()
{
    assert(eft_handon_weight(0.f, .1f) == 1.f);
    assert(eft_handon_weight(.05f, .1f) == .5f);
    assert(eft_handon_weight(.1f, .1f) == 0.f);
    assert(eft_handon_weight(.2f, .1f) == 0.f);
    assert(eft_handon_weight(-1.f, .1f) == 0.f);
    assert(eft_handon_weight(0.f, 0.f) == 0.f);
    assert(eft_handon_weight(std::numeric_limits<float>::quiet_NaN(), .1f) == 0.f);
    assert(eft_handon_weight(.05f, std::numeric_limits<float>::infinity()) == 0.f);
    struct V { float x, y, z; };
    const V keys[] = {{-.1f, 0.f, 0.f}, {.1f, 0.f, 0.f}};
    V delta;
    assert(eft_handon_sample(keys, 2, 1.f / 60.f, 30.f, delta));
    assert(std::abs(delta.x) < 1e-6f); // interpolate vectors, not weights
    assert(eft_handon_sample(keys, 2, 2.f / 30.f, 30.f, delta));
    assert(delta.x == -.1f); // same wrap as renderer, including live falloff
    assert(!eft_handon_sample(keys, 2, -1.f, 30.f, delta));
    assert(!eft_handon_sample(keys, 0, 0.f, 30.f, delta));
    assert(!eft_handon_sample(keys, 2, 0.f, 0.f, delta));
    assert(!eft_handon_sample(keys, 2, std::numeric_limits<float>::infinity(), 30.f, delta));
    EftHandOnMix mix;
    assert(mix.weight(.1f) == 0.f);
    assert(mix.add(-.1f, 0.f, 0.f, .5f)); // outgoing's ORIGINAL difference
    assert(mix.add(.1f, 0.f, 0.f, .5f));  // incoming; both independent existing clocks
    assert(mix.weight(.1f) == 1.f); // scalar influence interpolation would be0
    EftHandOnMix scaled;
    assert(scaled.add(.05f, 0.f, 0.f, .25f));
    assert(scaled.weight(.1f) == .5f); // normalize current blendAmount sum
    assert(!scaled.add(0.f, 0.f, 0.f, -1.f));
    return 0;
}
