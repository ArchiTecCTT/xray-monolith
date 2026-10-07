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
