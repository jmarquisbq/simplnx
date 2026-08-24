#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArrayCreationParameter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"

namespace nx::core
{

/**
 * @struct ComputeVectorColorsInputValues
 * @brief Input paths and options used to compute RGB colors from vector tuples.
 */
struct SIMPLNXCORE_EXPORT ComputeVectorColorsInputValues
{
  /**
   * @brief When true, only tuples with a nonzero mask value receive a color.
   */
  bool UseMask;
  /**
   * @brief Path to the float32 input array containing three components per tuple.
   */
  DataPath VectorsArrayPath;
  /**
   * @brief Path to the bool or uint8 mask array used when UseMask is true.
   */
  DataPath MaskArrayPath;
  /**
   * @brief Path where the three-component uint8 RGB output is written.
   */
  DataPath CellVectorColorsArrayPath;
};

/**
 * @class ComputeVectorColors
 * @brief Converts vector directions to RGB colors using bounded chunk buffers.
 *
 * The algorithm streams input vectors and an optional mask through fixed-size buffers before bulk-writing RGB tuples. This preserves the established color mapping while avoiding per-cell datastore
 * access for out-of-core arrays.
 */
class SIMPLNXCORE_EXPORT ComputeVectorColors
{
public:
  /**
   * @brief Constructs the vector-color algorithm.
   * @param dataStructure Data structure containing the input and output arrays.
   * @param mesgHandler Handler used for progress messages.
   * @param shouldCancel Cancellation flag checked between chunks.
   * @param inputValues Paths and options controlling the conversion.
   */
  ComputeVectorColors(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeVectorColorsInputValues* inputValues);

  /**
   * @brief Destroys the algorithm instance.
   */
  ~ComputeVectorColors() noexcept;

  /**
   * @brief Copy construction is disabled.
   */
  ComputeVectorColors(const ComputeVectorColors&) = delete;

  /**
   * @brief Move construction is disabled.
   */
  ComputeVectorColors(ComputeVectorColors&&) noexcept = delete;

  /**
   * @brief Copy assignment is disabled.
   */
  ComputeVectorColors& operator=(const ComputeVectorColors&) = delete;

  /**
   * @brief Move assignment is disabled.
   */
  ComputeVectorColors& operator=(ComputeVectorColors&&) noexcept = delete;

  /**
   * @brief Executes the bounded-memory vector-to-color conversion.
   * @return A valid result, or the first datastore/mask error encountered.
   */
  Result<> operator()();

  /**
   * @brief Returns the cancellation flag associated with this execution.
   * @return Reference to the cancellation flag.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const ComputeVectorColorsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
