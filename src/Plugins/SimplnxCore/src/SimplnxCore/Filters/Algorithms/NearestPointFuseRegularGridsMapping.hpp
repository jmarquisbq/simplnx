#pragma once

#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"

#include <limits>

namespace nx::core::grid_fusion
{
inline constexpr usize k_InvalidIndex = std::numeric_limits<usize>::max();

/**
 * @brief Holds the values that map reference cell centers to sampling cells along one axis.
 */
struct Axis
{
  float64 origin_offset;
  float64 reference_spacing;
  float64 sample_spacing;
  usize sample_dim;
};

/**
 * @brief Collects the mapping values of one axis from the two geometries.
 * @param reference Geometry whose cell centers are sampled.
 * @param sample Geometry that supplies the values.
 * @param axis Axis index (0 = X, 1 = Y, 2 = Z).
 * @return The mapping values. The origin difference is taken first so that a large common origin cancels.
 */
inline Axis MakeAxis(const ImageGeom& reference, const ImageGeom& sample, usize axis)
{
  return {.origin_offset = static_cast<float64>(reference.getOrigin()[axis]) - static_cast<float64>(sample.getOrigin()[axis]),
          .reference_spacing = static_cast<float64>(reference.getSpacing()[axis]),
          .sample_spacing = static_cast<float64>(sample.getSpacing()[axis]),
          .sample_dim = sample.getDimensions()[axis]};
}

/**
 * @brief Finds the sampling cell that contains the center of a reference cell.
 * @param axis Mapping values of the axis.
 * @param refIndex Reference cell index on the axis.
 * @return The sampling cell index, or k_InvalidIndex when the center is outside the sampling grid.
 *
 * The sampling grid includes its minimum face and excludes its maximum face. A center on a shared face belongs to the
 * cell with the higher index.
 */
inline usize GetSourceIndex(const Axis& axis, usize refIndex)
{
  const float64 position = (axis.origin_offset + (static_cast<float64>(refIndex) + 0.5) * axis.reference_spacing) / axis.sample_spacing;
  if(position >= 0.0 && position < static_cast<float64>(axis.sample_dim))
  {
    return static_cast<usize>(position);
  }
  return k_InvalidIndex;
}
} // namespace nx::core::grid_fusion
