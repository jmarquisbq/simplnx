#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Parameters/NumberParameter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"
#include "simplnx/Utilities/AlignSections.hpp"

#include <vector>

namespace nx::core
{

namespace compute_misorientations_constants
{

const ChoicesParameter::Choices k_ComputationTypeStrings = {"Use Arrays", "Use Reference Axis Angle"};
constexpr ChoicesParameter::ValueType k_UseArraysIndex = 0;
constexpr ChoicesParameter::ValueType k_UseReferenceAxesIndex = 1;

} // namespace compute_misorientations_constants

/**
 * @brief Input paths and computation settings consumed by ComputeMisorientations.
 *
 * Keeping these values separate from the algorithm object allows the filter to pass
 * validated arguments without coupling the implementation to parameter extraction.
 */
struct ORIENTATIONANALYSIS_EXPORT ComputeMisorientationsInputValues
{
  DataPath InputOrientationPath1;
  DataPath InputOrientationPath2;
  VectorFloat32Parameter::ValueType ReferenceOrientation;
  ChoicesParameter::ValueType ComputationType;
  DataPath InputPhasesArrayPath;
  DataPath OutputMisorientationsPath;
  DataPath InputCrystalStructuresArrayPath;
};

/**
 * @brief Computes an axis-angle misorientation for every input orientation tuple.
 *
 * Cell-level arrays are processed through bounded bulk-I/O buffers so the same
 * implementation remains efficient for in-core stores and avoids per-cell datastore
 * access for out-of-core stores. Ensemble crystal structures are cached locally because
 * they are small and repeatedly referenced by the cell loop.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeMisorientations
{
public:
  ComputeMisorientations(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, ComputeMisorientationsInputValues* inputValues);
  ~ComputeMisorientations() noexcept;

  ComputeMisorientations(const ComputeMisorientations&) = delete;
  ComputeMisorientations(ComputeMisorientations&&) noexcept = delete;
  ComputeMisorientations& operator=(const ComputeMisorientations&) = delete;
  ComputeMisorientations& operator=(ComputeMisorientations&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeMisorientationsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
