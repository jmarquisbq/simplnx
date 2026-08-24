#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/AttributeMatrixSelectionParameter.hpp"
#include "simplnx/Parameters/BoolParameter.hpp"
#include "simplnx/Parameters/DataObjectNameParameter.hpp"
#include "simplnx/Parameters/GeometrySelectionParameter.hpp"

namespace nx::core
{

/**
 * @struct ComputeFeatureSizesInputValues
 * @brief Holds all user-configured parameters for the ComputeFeatureSizes algorithm.
 */
struct SIMPLNXCORE_EXPORT ComputeFeatureSizesInputValues
{
  DataObjectNameParameter::ValueType EquivalentDiametersName;              ///< Output: equivalent spherical/circular diameter array name.
  AttributeMatrixSelectionParameter::ValueType FeatureAttributeMatrixPath; ///< Feature-level Attribute Matrix.
  ArraySelectionParameter::ValueType FeatureIdsPath;                       ///< Per-cell Feature ID array.
  GeometrySelectionParameter::ValueType InputImageGeometryPath;            ///< Input ImageGeom or RectGridGeom.
  DataObjectNameParameter::ValueType NumElementsName;                      ///< Output: per-feature voxel count array name.
  BoolParameter::ValueType SaveElementSizes;                               ///< If true, persist per-element sizes in the Geometry.
  DataObjectNameParameter::ValueType VolumesName;                          ///< Output: per-feature volume/area array name.
};

/**
 * @class ComputeFeatureSizes
 * @brief Dispatcher that selects between the in-core (Direct) and out-of-core (Scanline)
 * feature-size algorithms at runtime.
 *
 * This class contains no algorithm logic itself. Its operator()() inspects the storage
 * backing of the FeatureIds array and calls
 * `DispatchAlgorithm<ComputeFeatureSizesDirect, ComputeFeatureSizesScanline>(...)`.
 *
 * **Algorithm overview**: For each feature in an Image Geometry or Rectilinear Grid
 * Geometry, compute its volume (or area in 2D), equivalent spherical/circular diameter,
 * and voxel count.
 *
 * **Dispatch rules** (see AlgorithmDispatch.hpp):
 * - If the FeatureIds array is backed by in-memory DataStore, the Direct variant is used.
 *   It parallelizes the per-voxel counting/summation across Z-slices with thread-local
 *   accumulators.
 * - If the FeatureIds array uses out-of-core (chunked) storage, the Scanline variant is
 *   used. It streams FeatureIds (and element sizes for RectGrid) in fixed-size chunks via
 *   copyIntoBuffer() to avoid per-voxel chunk thrashing.
 * - Global test-override flags (ForceOocAlgorithm, ForceInCoreAlgorithm) can override the
 *   automatic detection for unit testing.
 *
 * @see ComputeFeatureSizesDirect, ComputeFeatureSizesScanline, DispatchAlgorithm
 */
class SIMPLNXCORE_EXPORT ComputeFeatureSizes
{
public:
  ComputeFeatureSizes(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeFeatureSizesInputValues* inputValues);
  ~ComputeFeatureSizes() noexcept;

  ComputeFeatureSizes(const ComputeFeatureSizes&) = delete;
  ComputeFeatureSizes(ComputeFeatureSizes&&) noexcept = delete;
  ComputeFeatureSizes& operator=(const ComputeFeatureSizes&) = delete;
  ComputeFeatureSizes& operator=(ComputeFeatureSizes&&) noexcept = delete;

  /**
   * @brief Dispatches to the Direct (in-core) or Scanline (out-of-core) variant based on
   * whether the FeatureIds array uses out-of-core storage.
   * @return Result<> indicating success or error.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;                                ///< Reference to the DataStructure.
  const ComputeFeatureSizesInputValues* m_InputValues = nullptr; ///< User-configured parameters.
  const std::atomic_bool& m_ShouldCancel;                        ///< Cancellation flag.
  const IFilter::MessageHandler& m_MessageHandler;               ///< Message handler for progress.
};

} // namespace nx::core
