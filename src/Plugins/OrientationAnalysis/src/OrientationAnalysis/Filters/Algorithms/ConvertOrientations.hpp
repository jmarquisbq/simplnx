#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Parameters/DataObjectNameParameter.hpp"

#include <EbsdLib/Orientation/OrientationFwd.hpp>

#include <chrono>
#include <concepts>
#include <mutex>

namespace nx::core
{

namespace convert_orientations_constants
{
// Error Code constants
constexpr int32 k_InputRepresentationTypeError = -67001;
constexpr int32 k_OutputRepresentationTypeError = -67002;
constexpr int32 k_InputComponentDimensionError = -67003;
constexpr int32 k_InputComponentCountError = -67004;
constexpr int32 k_MatchingTypesError = -67005;
} // namespace convert_orientations_constants

/**
 * @brief Input values for the ConvertOrientations algorithm.
 */
struct ORIENTATIONANALYSIS_EXPORT ConvertOrientationsInputValues
{
  ArraySelectionParameter::ValueType InputOrientationArrayPath;  ///< Cell-level Float32 input orientation array
  ebsdlib::orientations::Type InputType;                         ///< Enumerated input representation type
  DataObjectNameParameter::ValueType OutputOrientationArrayName; ///< Name for the output orientation array
  ebsdlib::orientations::Type OutputType;                        ///< Enumerated output representation type
};

/**
 * @class ConvertOrientations
 * @brief Converts between orientation representations (Euler angles, quaternions,
 *        orientation matrices, axis-angle, Rodrigues, homochoric, cubochoric,
 *        and stereographic projection).
 *
 * A macro-generated parallel worker class is instantiated for each valid
 * input/output combination. The worker reads input tuples, converts each
 * orientation, and writes the result to the output array.
 *
 * ## OOC Optimization
 *
 * The macro-generated parallel worker classes now use chunked bulk I/O
 * internally (chunk size of 4096 tuples). Within each `operator()(Range)`
 * call, input data is read via `copyIntoBuffer()` and output data is written
 * via `copyFromBuffer()` in chunks, with the conversion loop operating on
 * contiguous local buffers. This replaces per-element `operator[]` access
 * that would trigger chunk load/evict cycles with OOC storage.
 */
class ORIENTATIONANALYSIS_EXPORT ConvertOrientations
{
public:
  ConvertOrientations(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ConvertOrientationsInputValues* inputValues);
  ~ConvertOrientations() noexcept;

  ConvertOrientations(const ConvertOrientations&) = delete;
  ConvertOrientations(ConvertOrientations&&) noexcept = delete;
  ConvertOrientations& operator=(const ConvertOrientations&) = delete;
  ConvertOrientations& operator=(ConvertOrientations&&) noexcept = delete;

  /**
   * @brief Executes the orientation conversion using parallel chunked bulk I/O.
   * @return Result<> with any errors encountered during execution.
   */
  Result<> operator()();

  /**
   * @brief Returns true if the user has requested cancellation.
   */
  bool shouldCancel() const;

  /**
   * @brief Emits mutex-protected, time-throttled progress updates from parallel workers.
   * @param counter Number of tuples completed by the caller.
   */
  void sendThreadSafeProgressMessage(usize counter);

  /**
   * @brief Exposes the cancellation flag so streaming workers can poll it.
   */
  const std::atomic_bool& getCancel() const
  {
    return m_ShouldCancel;
  }

private:
  DataStructure& m_DataStructure;
  const ConvertOrientationsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;

  std::chrono::steady_clock::time_point m_InitialPoint = std::chrono::steady_clock::now();
  mutable std::mutex m_ProgressMessage_Mutex;
  usize m_TotalPoints = 0;
  usize m_ProgressCounter = 0;
};

} // namespace nx::core
