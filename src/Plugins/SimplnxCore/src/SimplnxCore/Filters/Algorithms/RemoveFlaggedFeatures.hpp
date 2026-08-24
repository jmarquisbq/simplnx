#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/MultiArraySelectionParameter.hpp"

namespace nx::core
{

enum class Functionality : uint64
{
  Remove = 0,
  Extract = 1,
  ExtractThenRemove = 2,
};

struct SIMPLNXCORE_EXPORT RemoveFlaggedFeaturesInputValues
{
  bool FillRemovedFeatures;
  uint64 ExtractFeatures;
  DataPath FeatureIdsArrayPath;
  DataPath FlaggedFeaturesArrayPath;
  DataPath ImageGeometryPath;
  DataPath TempBoundsPath;
  std::string CreatedImageGeometryPrefix;
  MultiArraySelectionParameter::ValueType IgnoredDataArrayPaths;
};

/**
 * @class RemoveFlaggedFeatures
 * @brief Dispatcher that selects between the in-core (Direct) and out-of-core (Scanline)
 * remove/extract-flagged-features algorithms at runtime.
 *
 * This class does not contain any algorithm logic itself. Its operator()() inspects
 * the storage backing of the FeatureIds array and calls
 * `DispatchAlgorithm<RemoveFlaggedFeaturesDirect, RemoveFlaggedFeaturesScanline>(...)`.
 *
 * **Algorithm overview**: Depending on the selected Functionality, this removes flagged
 * Features from the FeatureIds array (optionally filling the resulting gaps by majority
 * vote of face-neighbors), extracts flagged Features into new cropped ImageGeom(s), or
 * both.
 *
 * **Dispatch rules** (see AlgorithmDispatch.hpp):
 * - If the FeatureIds array is backed by in-memory DataStore, the Direct variant is used.
 * - If FeatureIds uses out-of-core (chunked) storage, the Scanline variant is used to
 *   avoid random-access chunk thrashing during the neighbor-fill loop.
 * - Global test-override flags (ForceOocAlgorithm, ForceInCoreAlgorithm) can override
 *   the automatic detection for unit testing purposes.
 *
 * @see RemoveFlaggedFeaturesDirect, RemoveFlaggedFeaturesScanline, DispatchAlgorithm
 */
class SIMPLNXCORE_EXPORT RemoveFlaggedFeatures
{
public:
  RemoveFlaggedFeatures(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, RemoveFlaggedFeaturesInputValues* inputValues);
  ~RemoveFlaggedFeatures() noexcept;

  RemoveFlaggedFeatures(const RemoveFlaggedFeatures&) = delete;
  RemoveFlaggedFeatures(RemoveFlaggedFeatures&&) noexcept = delete;
  RemoveFlaggedFeatures& operator=(const RemoveFlaggedFeatures&) = delete;
  RemoveFlaggedFeatures& operator=(RemoveFlaggedFeatures&&) noexcept = delete;

  /**
   * @brief Dispatches to the appropriate algorithm variant (Direct or Scanline)
   * based on whether the FeatureIds array uses out-of-core storage.
   * @return Result<> indicating success or any errors encountered.
   */
  Result<> operator()();

  /**
   * @brief Returns a reference to the cancellation flag.
   * @return Const reference to the atomic cancellation boolean.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const RemoveFlaggedFeaturesInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
