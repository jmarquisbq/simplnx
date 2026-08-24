#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArrayCreationParameter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/DataGroupCreationParameter.hpp"
#include "simplnx/Parameters/NumberParameter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"
#include "simplnx/Utilities/SampleSurfaceMesh.hpp"

#include <random>

namespace nx::core
{
struct SIMPLNXCORE_EXPORT UncertainRegularGridSampleSurfaceMeshInputValues
{
  uint64 SeedValue;
  VectorUInt64Parameter::ValueType Dimensions;
  VectorFloat32Parameter::ValueType Spacing;
  VectorFloat32Parameter::ValueType Origin;
  VectorFloat32Parameter::ValueType Uncertainty;
  DataPath TriangleGeometryPath;
  DataPath SurfaceMeshFaceLabelsArrayPath;
  DataPath FeatureIdsArrayPath;
};

/**
 * @class UncertainRegularGridSampleSurfaceMesh
 * @brief Samples a TriangleGeometry onto a regular grid whose sample points are
 * jittered by a per-axis "uncertainty" offset drawn from a seeded pseudo-random
 * generator. Unlike RegularGridSampleSurfaceMesh's deterministic slice-plane
 * rasterization, the jittered point positions require an actual
 * point-in-polyhedron test per sample (the base SampleSurfaceMesh class), so
 * this class only supplies the streaming point generation: one Z-slice of
 * jittered points at a time, continuing the same generator across slices so
 * the draw sequence exactly matches a single monolithic full-volume pass.
 */
class SIMPLNXCORE_EXPORT UncertainRegularGridSampleSurfaceMesh : public SampleSurfaceMesh
{
public:
  UncertainRegularGridSampleSurfaceMesh(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                        UncertainRegularGridSampleSurfaceMeshInputValues* inputValues);
  ~UncertainRegularGridSampleSurfaceMesh() noexcept override;

  UncertainRegularGridSampleSurfaceMesh(const UncertainRegularGridSampleSurfaceMesh&) = delete;
  UncertainRegularGridSampleSurfaceMesh(UncertainRegularGridSampleSurfaceMesh&&) noexcept = delete;
  UncertainRegularGridSampleSurfaceMesh& operator=(const UncertainRegularGridSampleSurfaceMesh&) = delete;
  UncertainRegularGridSampleSurfaceMesh& operator=(UncertainRegularGridSampleSurfaceMesh&&) noexcept = delete;

  Result<> operator()();

  const std::atomic_bool& getCancel();

protected:
  SizeVec3 getGridDimensions() const override;
  void generateSlicePoints(usize zSlice, std::vector<Point3Df>& slicePoints) override;

private:
  DataStructure& m_DataStructure;
  const UncertainRegularGridSampleSurfaceMeshInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;

  // Persistent pseudo-random state, advanced one Z-slice at a time by
  // generateSlicePoints(). This must not be reset or re-seeded between
  // slices: the base class calls generateSlicePoints() once per Z-slice in
  // increasing order, and the resulting draw sequence (one Z draw per slice,
  // one Y draw per row, one X draw per point) must reproduce exactly the
  // sequence a single monolithic full-volume generation pass would have
  // produced, so results stay identical for a given seed.
  std::mt19937 m_Generator;
  std::uniform_real_distribution<float32> m_Distribution{0.0F, 1.0F};
};
} // namespace nx::core
