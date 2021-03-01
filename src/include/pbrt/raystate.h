#ifndef PBRT_CLOUD_RAYSTATE_H
#define PBRT_CLOUD_RAYSTATE_H

#include <memory>

#include "geometry.h"
#include "transform.h"
#include "spectrum.h"

namespace pbrt {

struct RayState;
using RayStatePtr = std::unique_ptr<RayState>;

class RayState {
  public:
    struct __attribute__((packed)) TreeletNode {
        uint32_t treelet{0};
        uint32_t node{0};
        uint8_t primitive{0};
        bool transformed{false};
    };

    struct Sample {
        uint64_t id;
        Point2f pFilm;
        Float weight;
        int dim;
    };

    RayState() = default;
    RayState(RayState &&) = default;

    /* disallow copying */
    RayState(const RayState &) = delete;
    RayState &operator=(const RayState &) = delete;

    bool trackRay{false};
    mutable uint16_t hop{0};
    mutable uint16_t pathHop{0};

    Sample sample;
    RayDifferential ray;
    Spectrum beta{1.f};
    Spectrum Ld{0.f};
    uint8_t remainingBounces{3};
    bool isShadowRay{false};

    bool hit{false};
    TreeletNode hitNode{};

    Transform hitTransform{};
    Transform rayTransform{};

    uint8_t toVisitHead{0};
    TreeletNode toVisit[64];

    static const size_t MaxPackedSize;

    bool IsShadowRay() const { return isShadowRay; }
    bool HasHit() const { return hit; }

    int64_t SampleNum(const uint32_t spp) const;
    Point2i SamplePixel(const Vector2i &extent, const uint32_t spp) const;

    bool toVisitEmpty() const { return toVisitHead == 0; }
    const TreeletNode &toVisitTop() const { return toVisit[toVisitHead - 1]; }
    void toVisitPush(TreeletNode &&t) { toVisit[toVisitHead++] = std::move(t); }
    void toVisitPop() { toVisitHead--; }

    void SetHit(const TreeletNode &node);
    void StartTrace();
    uint32_t CurrentTreelet() const;

    uint64_t PathID() const { return sample.id; }

    /* serialization */
    size_t Serialize(char *data);
    void Deserialize(const char *data, const size_t len);

    size_t MaxSize() const;
    size_t MaxCompressedSize() const;

    static RayStatePtr Create();

    int touchRay() const {
      int sum = isShadowRay + hit;
      for (int j = toVisitHead - 1; j >= 0; j--)
        sum += toVisit[j].treelet + toVisit[j].node + toVisit[j].primitive + toVisit[j].transformed;
      return sum;
    }
};

class Sample {
  public:
    uint64_t sampleId{};
    Point2f pFilm{};
    Float weight{};
    Spectrum L{};

    /* Sample is serialized up to this point */
    Sample() = default;
    Sample(const RayState &rayState);
    Sample(Sample &&) = default;

    /* disallow copying */
    Sample(const Sample &) = delete;
    Sample &operator=(const Sample &) = delete;

    int64_t SampleNum(const uint32_t spp) const;
    Point2i SamplePixel(const Vector2i &extent, const uint32_t spp) const;

    size_t Size() const;
    size_t Serialize(char *data);
    void Deserialize(const char *data, const size_t len);

    size_t MaxCompressedSize() const { return Size(); }
};

}  // namespace pbrt

#endif /* PBRT_CLOUD_RAYSTATE_H */
