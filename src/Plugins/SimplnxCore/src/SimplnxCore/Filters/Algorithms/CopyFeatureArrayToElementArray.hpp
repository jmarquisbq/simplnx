#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/MultiArraySelectionParameter.hpp"
#include "simplnx/Parameters/StringParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT CopyFeatureArrayToElementArrayInputValues
{
  StringParameter::ValueType CreatedArraySuffix;
  ArraySelectionParameter::ValueType FeatureIdsPath;
  MultiArraySelectionParameter::ValueType SelectedFeatureArrayPaths;
};

/**
 * @class CopyFeatureArrayToElementArray
 * @brief Dispatcher that selects between the in-core (Direct) and out-of-core (Scanline)
 * algorithms for broadcasting feature data down to element (cell) data.
 *
 * This class contains no algorithm logic itself. Its operator()() inspects the storage backing
 * of the FeatureIds array and calls
 * `DispatchAlgorithm<CopyFeatureArrayToElementArrayDirect, CopyFeatureArrayToElementArrayScanline>(...)`.
 *
 * **Algorithm overview**: For each selected feature-level array, create a cell-level array where
 * every cell receives the value of the feature it belongs to
 * (created[cell] = selectedFeature[featureIds[cell]]).
 *
 * **Dispatch rules** (see AlgorithmDispatch.hpp):
 * - If the FeatureIds array is backed by in-memory DataStore, the Direct (parallel) variant is used.
 * - If it uses out-of-core (chunked) storage, the Scanline variant is used to avoid unsafe parallel
 *   chunk-cache access and per-element chunk lookups.
 * - Global test-override flags (ForceOocAlgorithm, ForceInCoreAlgorithm) can override the automatic
 *   detection for unit testing.
 *
 * @see CopyFeatureArrayToElementArrayDirect, CopyFeatureArrayToElementArrayScanline, DispatchAlgorithm
 */
class SIMPLNXCORE_EXPORT CopyFeatureArrayToElementArray
{
public:
  CopyFeatureArrayToElementArray(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                 const CopyFeatureArrayToElementArrayInputValues* inputValues);
  ~CopyFeatureArrayToElementArray() noexcept;

  CopyFeatureArrayToElementArray(const CopyFeatureArrayToElementArray&) = delete;
  CopyFeatureArrayToElementArray(CopyFeatureArrayToElementArray&&) noexcept = delete;
  CopyFeatureArrayToElementArray& operator=(const CopyFeatureArrayToElementArray&) = delete;
  CopyFeatureArrayToElementArray& operator=(CopyFeatureArrayToElementArray&&) noexcept = delete;

  /**
   * @brief Runs the copy for every selected feature array.
   * @return Invalid Result on FeatureIds range-validation failure (-5355 / -5351).
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const CopyFeatureArrayToElementArrayInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
