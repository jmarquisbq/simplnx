#pragma once

#include "simplnx/Common/Result.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/simplnx_export.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

#include <nonstd/span.hpp>

namespace nx::core
{
/**
 * @brief Compares two complete fixed-width records.
 * @return A negative, zero, or positive value for left-before, equivalent, or left-after. Equivalent records retain append order.
 */
using ExternalSortCompare = std::function<int32(nonstd::span<const std::byte> left, nonstd::span<const std::byte> right)>;
/** @brief Reports operation-local completed and total records; implementations guarantee completed is never greater than total. */
using ExternalSortProgressCallback = std::function<void(uint64 completedRecords, uint64 totalRecords)>;

/**
 * @struct ExternalSortConfig
 * @brief Defines the fixed record width, largest caller batch, and stable comparison used by an external sort.
 */
struct SIMPLNX_EXPORT ExternalSortConfig
{
  uint64 recordSize = 0;         ///< Exact width in bytes of every appended and returned record.
  uint64 maxRecordsPerBatch = 0; ///< Maximum record count accepted by a single append or read request.
  ExternalSortCompare compare;   ///< Deterministic record comparator; equal records retain append order.
};

/**
 * @brief Produces and repeatedly reads a stable, deterministically sorted stream of fixed-size records.
 * Callers own bounded append/read batches. Append then finish before read; reads may repeat. A terminal append/finish
 * cancellation or error invalidates the instance, while a cancelled read is retryable. Implementations retain bounded storage.
 */
class SIMPLNX_EXPORT IExternalSort
{
public:
  virtual ~IExternalSort() noexcept = default;

  /**
   * @brief Appends unsorted records before finish() is called.
   * @param recordCount Number of complete records in records.
   * @param records Caller-owned bytes containing exactly recordCount fixed-width records.
   * @param shouldCancel Cancellation flag; cancellation makes the append/finish lifecycle terminal.
   * @param progressCallback Optional operation-local progress callback.
   * @return A valid result or a configuration, size, storage, lifecycle, or cancellation error.
   */
  virtual Result<> append(uint64 recordCount, nonstd::span<const std::byte> records, const std::atomic_bool& shouldCancel, const ExternalSortProgressCallback& progressCallback) = 0;
  /**
   * @brief Completes stable sorting and transitions the instance to repeatable read mode.
   * @param shouldCancel Cancellation flag; a cancelled finish invalidates the instance.
   * @param progressCallback Optional operation-local progress callback.
   * @return A valid result or the sort/provider/cancellation failure.
   */
  virtual Result<> finish(const std::atomic_bool& shouldCancel, const ExternalSortProgressCallback& progressCallback) = 0;
  /**
   * @brief Reads a bounded range from the completed sorted stream.
   * @param recordOffset Zero-based sorted record offset.
   * @param recordCount Maximum number of records requested.
   * @param records Caller-owned output buffer large enough for recordCount records.
   * @param shouldCancel Cancellation flag; unlike append/finish, a cancelled read may be retried.
   * @return The number of records read, which may be shorter only at end of stream, or an error.
   */
  virtual Result<uint64> read(uint64 recordOffset, uint64 recordCount, nonstd::span<std::byte> records, const std::atomic_bool& shouldCancel) const = 0;
  /** @brief Returns the number of records in the completed sorted stream. */
  virtual uint64 recordCount() const = 0;
};
} // namespace nx::core
