#pragma once

#include "simplnx/Common/Array.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/Geometry/VertexGeom.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Filter/Arguments.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/simplnx_export.hpp"

namespace nx::core
{
struct SIMPLNX_EXPORT SampleSurfaceMeshInputValues
{
  DataPath TriangleGeometryPath;
  DataPath SurfaceMeshFaceLabelsArrayPath;
  DataPath FeatureIdsArrayPath; // Make sure it's been initialized with zeroes
};

/**
 * @class SampleSurfaceMesh
 * @brief Determines, for every cell of a sampling grid, which enclosed feature
 * (if any) of a triangle surface mesh contains that cell's sample point, using
 * ray-cast point-in-polyhedron tests against each feature's bounding faces.
 *
 * The full set of sample points is one point per grid cell, optionally
 * perturbed by a subclass-specific rule (e.g. random uncertainty offsets).
 * Rather than materializing every sample point up front -- an allocation
 * proportional to the total cell count of the sampling grid -- this class
 * streams the point-in-polyhedron test one Z-slice at a time: a subclass
 * reports the grid's dimensions and fills in exactly one slice's worth of
 * points per call, in increasing Z order. Peak memory therefore stays bounded
 * to a single slice regardless of the sampling grid's total size, and the
 * resulting FeatureIds are written back with one bulk copyFromBuffer call per
 * slice instead of one random-access write per cell.
 */
class SIMPLNX_EXPORT SampleSurfaceMesh
{
public:
  SampleSurfaceMesh(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler);
  virtual ~SampleSurfaceMesh() noexcept;

  SampleSurfaceMesh(const SampleSurfaceMesh&) = delete;            // Copy Constructor Not Implemented
  SampleSurfaceMesh(SampleSurfaceMesh&&) = delete;                 // Move Constructor Not Implemented
  SampleSurfaceMesh& operator=(const SampleSurfaceMesh&) = delete; // Copy Assignment Not Implemented
  SampleSurfaceMesh& operator=(SampleSurfaceMesh&&) = delete;      // Move Assignment Not Implemented

  /**
   * @brief execute
   * @param gridGeom
   * @return
   */
  Result<> execute(SampleSurfaceMeshInputValues& inputValues);

  /**
   * @brief Returns the sampling grid's dimensions (X, Y, Z) in cells. The
   * base class drives the streaming point-in-polyhedron loop entirely from
   * this value, so it never needs the full point set in memory at once.
   */
  virtual SizeVec3 getGridDimensions() const = 0;

  /**
   * @brief Fills slicePoints with the sample point for every cell of Z-slice
   * zSlice, in row-major (X fastest) order. slicePoints is pre-sized to
   * getGridDimensions().getX() * getGridDimensions().getY() by the caller;
   * implementations must fill it by index rather than resize it.
   *
   * Called once per slice, strictly in increasing zSlice order. Implementations
   * that draw from a shared pseudo-random generator must continue that
   * generator's draw sequence across calls so the overall draw order
   * reproduces exactly the sequence of a single monolithic full-volume
   * generation pass.
   */
  virtual void generateSlicePoints(usize zSlice, std::vector<Point3Df>& slicePoints) = 0;

private:
  DataStructure& m_DataStructure;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
  MessageHelper m_MessageHelper;
};
} // namespace nx::core
