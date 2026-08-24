#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArrayCreationParameter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/BoolParameter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"

#include <atomic>

namespace nx::core
{

/**
 * @struct ConvertQuaternionInputValues
 * @brief Contains the paths and conversion options consumed by ConvertQuaternion.
 */
struct ORIENTATIONANALYSIS_EXPORT ConvertQuaternionInputValues
{
  DataPath QuaternionDataArrayPath;
  DataPath OutputDataArrayPath;
  bool DeleteOriginalData;
  ChoicesParameter::ValueType ConversionType;
};

/**
 * @class ConvertQuaternion
 * @brief Dispatches quaternion component-order conversion by storage type.
 *
 * Contiguous in-memory arrays use a parallel pointer-based implementation. Chunked arrays use
 * fixed-size bulk read/convert/write batches, keeping memory independent of tuple count.
 */
class ORIENTATIONANALYSIS_EXPORT ConvertQuaternion
{
public:
  /**
   * @brief Constructs the quaternion conversion algorithm.
   * @param dataStructure Data structure containing the input and output arrays.
   * @param messageHandler Handler used for progress messages.
   * @param shouldCancel Cancellation flag checked between bounded work blocks.
   * @param inputValues Conversion parameters that must outlive this object.
   */
  ConvertQuaternion(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, ConvertQuaternionInputValues* inputValues);

  /**
   * @brief Destroys the algorithm object.
   */
  ~ConvertQuaternion() noexcept;

  /**
   * @brief Copy construction is disabled because the object stores borrowed references.
   */
  ConvertQuaternion(const ConvertQuaternion&) = delete;

  /**
   * @brief Move construction is disabled because the object stores borrowed references.
   */
  ConvertQuaternion(ConvertQuaternion&&) noexcept = delete;

  /**
   * @brief Copy assignment is disabled because the object stores borrowed references.
   */
  ConvertQuaternion& operator=(const ConvertQuaternion&) = delete;

  /**
   * @brief Move assignment is disabled because the object stores borrowed references.
   */
  ConvertQuaternion& operator=(ConvertQuaternion&&) noexcept = delete;

  /**
   * @brief Converts every quaternion to the requested component order.
   * @return A valid result on success or a datastore/type error.
   */
  Result<> operator()();

  /**
   * @brief Returns the shared cancellation flag.
   * @return Reference to the cancellation flag supplied by the filter.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const ConvertQuaternionInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
