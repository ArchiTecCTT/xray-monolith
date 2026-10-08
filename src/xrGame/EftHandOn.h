#pragma once
#include <cmath>

// Recovered rule: EFT.Player.method_20, original animated IK/hand markers.
// Range is profile data (Tarkov 0.1 metres), NOT a fade time. The existing
// native clocks/amounts feed this independent, allocation-free arithmetic.
inline float eft_handon_weight(float distance, float range)
{
    if (!std::isfinite(distance) || !std::isfinite(range) || distance < 0.f || range <= 0.f)
        return 0.f;
    const float weight = 1.f - distance / range;
    return weight < 0.f ? 0.f : weight > 1.f ? 1.f : weight;
}

// Profile key `blend_time` (seconds for a full 0<->1 swing of the weight). It is
// OUR transition speed between two authored poses, not a Tarkov number; 0 gives
// back the raw Tarkov rule above, frame for frame.
const float EFT_HANDON_BLEND_TIME_DEFAULT = .25f;
const float EFT_HANDON_BLEND_TIME_MAX = 2.f;
inline bool eft_handon_blend_time_valid(float t)
{
    return std::isfinite(t) && t >= 0.f && t <= EFT_HANDON_BLEND_TIME_MAX;
}

// Rate limit on the weight, applied AFTER the Tarkov rule. The raw weight goes
// 1->0 (or 0->1) in 4-5 frames when the hand leaves/reaches the grip; the gap
// between the grip's hold and the plain clip hand then closes (or opens) in
// ~0.15 s on top of the clip's own motion, which reads as a flick through the
// default pose. Limiting the slope spreads that gap over `blend_time`. Both
// directions are limited, same rate. Exact raw value whenever the raw weight
// moves slower than the limit, so the steady-state rule stays Tarkov's.
// `level` is -1 when the layer was not live last frame: then it starts AT the
// raw weight (no history to ease from; never worse than the raw rule).
struct EftHandOnEase
{
    float level = -1.f;
    unsigned last = ~0u;  // frame of the last real step
    unsigned frame = ~0u; // one step per rendered frame; repeat calls reuse the result
    void reset() { level = -1.f; frame = last = ~0u; }
    float step(float raw, float dt, float blend_time, unsigned frame_no)
    {
        if (!std::isfinite(raw)) raw = 0.f;
        raw = raw < 0.f ? 0.f : raw > 1.f ? 1.f : raw;
        if (!eft_handon_blend_time_valid(blend_time) || blend_time == 0.f)
        {
            frame = frame_no;
            return level = raw; // limiter off: the raw rule, nothing remembered but the value
        }
        if (frame == frame_no && level >= 0.f) return level;
        frame = frame_no;
        // No history, or update_handon was not called for a while (script/offhand ownership): start at raw.
        if (!(level >= 0.f) || !std::isfinite(level) || frame_no - last > 2u) { last = frame_no; return level = raw; }
        last = frame_no;
        const float max_step = (std::isfinite(dt) && dt > 0.f) ? dt / blend_time : 0.f;
        if (raw > level) level = raw - level > max_step ? level + max_step : raw;
        else level = level - raw > max_step ? level - max_step : raw;
        return level;
    }
};

// Match renderer's key interpolation/wrap; no second clock, seek or callback.
template<class Vector>
inline bool eft_handon_sample(const Vector* vectors, unsigned count, float time, float fps, Vector& delta)
{
    const float frame = time * fps;
    if (!vectors || !count || !std::isfinite(frame) || time < 0.f || fps <= 0.f) return false;
    const float whole = std::floor(frame);
    const unsigned first = unsigned(std::fmod(whole, float(count)));
    const unsigned next = (first + 1) % count;
    const float part = frame - whole;
    delta.x = vectors[first].x + part * (vectors[next].x - vectors[first].x);
    delta.y = vectors[first].y + part * (vectors[next].y - vectors[first].y);
    delta.z = vectors[first].z + part * (vectors[next].z - vectors[first].z);
    return std::isfinite(delta.x) && std::isfinite(delta.y) && std::isfinite(delta.z);
}

struct EftHandOnMix
{
    float x = 0.f, y = 0.f, z = 0.f, total = 0.f;
    bool add(float dx, float dy, float dz, float amount)
    {
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
            !std::isfinite(amount) || amount < 0.f) return false;
        x += dx * amount; y += dy * amount; z += dz * amount; total += amount;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(total);
    }
    float weight(float range) const
    {
        if (total <= 0.f) return 0.f;
        const float dx = x / total, dy = y / total, dz = z / total;
        return eft_handon_weight(std::sqrt(dx * dx + dy * dy + dz * dz), range);
    }
};
