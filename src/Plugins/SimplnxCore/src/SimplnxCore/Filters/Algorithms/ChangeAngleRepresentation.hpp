#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"

namespace nx::core
{

/**
 * @brief Runtime values used by ChangeAngleRepresentation.
 */
struct SIMPLNXCORE_EXPORT ChangeAngleRepresentationInputValues
{
  ArraySelectionParameter::ValueType AnglesArrayPath;
  ChoicesParameter::ValueType ConversionTypeIndex;
};

/**
 * @class ChangeAngleRepresentation
 * @brief Converts float32 angle values in place using storage-aware execution.
 *
 * Contiguous in-memory stores use direct parallel multiplication. Out-of-core
 * stores stream through a fixed-size buffer using bulk datastore I/O.
 */

class SIMPLNXCORE_EXPORT ChangeAngleRepresentation
{
public:
  ChangeAngleRepresentation(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ChangeAngleRepresentationInputValues* inputValues);
  ~ChangeAngleRepresentation() noexcept;

  ChangeAngleRepresentation(const ChangeAngleRepresentation&) = delete;
  ChangeAngleRepresentation(ChangeAngleRepresentation&&) noexcept = delete;
  ChangeAngleRepresentation& operator=(const ChangeAngleRepresentation&) = delete;
  ChangeAngleRepresentation& operator=(ChangeAngleRepresentation&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ChangeAngleRepresentationInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
