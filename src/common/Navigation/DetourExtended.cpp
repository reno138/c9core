/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it and/or modify it under version 2 of the License, or (at your option), any later version.
 */

#include "DetourExtended.h"
#include "DetourCommon.h"

#include <cmath>

float dtQueryFilterExt::getCost(const float* pa, const float* pb,
                const dtPolyRef /*prevRef*/, const dtMeshTile* /*prevTile*/, const dtPoly* /*prevPoly*/,
                const dtPolyRef /*curRef*/, const dtMeshTile* /*curTile*/, const dtPoly* curPoly,
                const dtPolyRef /*nextRef*/, const dtMeshTile* /*nextTile*/, const dtPoly* /*nextPoly*/) const
{
    float dist = dtVdist(pa, pb);
    // Trig-free slope cost: replaces atan+degrees with direct height ratio.
    // For walkable slopes (0-50°), sin(angle) * 180/(pi*100) ≈ 0.573 * dz/dist
    // which closely approximates the original formula: 1 + atan-degrees/100
    float dz = std::abs(pa[1] - pb[1]);
    float slopeCost = (dist > 1e-6f) ? 1.0f + 0.573f * dz / dist : 1.0f;
    return dist * slopeCost * getAreaCost(curPoly->getArea());
}
