#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct SilhouetteInputValues;

/**
 * @class SilhouetteDirect
 * @brief Computes per-tuple silhouette scores by directly indexing resident arrays.
 *
 * This implementation preserves the original all-pairs calculation for in-memory
 * data. Direct indexing is inexpensive when the input, feature IDs, mask, and
 * output are resident, but its tuple-sized distance workspaces and random access
 * pattern are not appropriate for disk-backed stores. Silhouette dispatches here
 * only when every participating array can use the in-core path.
 *
 * The object borrows the DataStructure and input-values bundle for its lifetime;
 * it neither owns nor outlives them.
 *
 * @see SilhouetteScanline for the bounded-memory OOC implementation.
 */
class SIMPLNXCORE_EXPORT SilhouetteDirect
{
public:
  /**
   * @brief Creates the in-core silhouette implementation.
   * @param dataStructure Data structure containing the clustering input, feature IDs, optional mask, and output.
   * @param messageHandler Unused by this direct implementation; accepted to keep both dispatched implementations interchangeable.
   * @param shouldCancel Unused by the preserved direct calculation; accepted as part of the common algorithm interface.
   * @param inputValues Non-owning pointer to the filter arguments. It must remain valid through operator()().
   */
  SilhouetteDirect(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, const SilhouetteInputValues* inputValues);
  ~SilhouetteDirect() noexcept;

  SilhouetteDirect(const SilhouetteDirect&) = delete;
  SilhouetteDirect(SilhouetteDirect&&) noexcept = delete;
  SilhouetteDirect& operator=(const SilhouetteDirect&) = delete;
  SilhouetteDirect& operator=(SilhouetteDirect&&) noexcept = delete;

  /**
   * @brief Computes each enabled tuple's within-cluster and nearest-other-cluster mean distances.
   * @return A valid result on completion, or an error when the optional mask cannot be instantiated.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const SilhouetteInputValues* m_InputValues = nullptr;
};
} // namespace nx::core
