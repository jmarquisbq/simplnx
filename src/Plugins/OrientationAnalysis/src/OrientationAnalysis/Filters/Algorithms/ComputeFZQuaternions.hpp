#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/BoolParameter.hpp"
#include "simplnx/Parameters/DataObjectNameParameter.hpp"

namespace nx::core
{

/**
 * @brief Input paths and options consumed by ComputeFZQuaternions.
 */
struct ORIENTATIONANALYSIS_EXPORT ComputeFZQuaternionsInputValues
{
  ArraySelectionParameter::ValueType CellPhasesArrayPath;
  ArraySelectionParameter::ValueType CrystalStructuresArrayPath;
  ArraySelectionParameter::ValueType InputQuatsArrayPath;
  ArraySelectionParameter::ValueType MaskArrayPath;
  DataObjectNameParameter::ValueType OutputFzQuatsArrayName;
  BoolParameter::ValueType UseMask;
};

/**
 * @class ComputeFZQuaternions
 * @brief Computes a symmetry-equivalent fundamental-zone quaternion for each input tuple.
 *
 * The algorithm dispatches between a parallel contiguous-store path for in-memory arrays and
 * a bounded streaming path for OOC arrays. This removes datastore abstraction from the direct
 * hot loop while preventing per-cell access from loading and evicting disk-backed chunks.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeFZQuaternions
{
public:
  ComputeFZQuaternions(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeFZQuaternionsInputValues* inputValues);
  ~ComputeFZQuaternions() noexcept;

  ComputeFZQuaternions(const ComputeFZQuaternions&) = delete;
  ComputeFZQuaternions(ComputeFZQuaternions&&) noexcept = delete;
  ComputeFZQuaternions& operator=(const ComputeFZQuaternions&) = delete;
  ComputeFZQuaternions& operator=(ComputeFZQuaternions&&) noexcept = delete;

  /**
   * @brief Executes the direct or streaming implementation based on the participating datastores.
   * @return A valid result on success or cancellation, or an error for invalid phase references or bulk I/O failures.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeFZQuaternionsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
