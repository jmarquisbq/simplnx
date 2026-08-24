#pragma once

#include "simplnx/Common/Result.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/simplnx_export.hpp"

#include <atomic>
#include <cstddef>
#include <memory>

#include <nonstd/span.hpp>

namespace nx::core
{
/**
 * @struct TemporaryRecordStoreConfig
 * @brief Configures a storage-neutral, fixed-width algorithm scratch store.
 */
struct SIMPLNX_EXPORT TemporaryRecordStoreConfig
{
  uint64 recordSize = 0;         ///< Exact byte width of each logical record.
  uint64 maxRecordsPerBatch = 0; ///< Largest record count accepted by one read or write call.
  uint64 initialRecordCount = 0; ///< Initial logical capacity in fixed-width records.
  bool readOnly = false;         ///< When true, write, fill, and resize requests must fail.
};

/**
 * @brief Storage-neutral RAII scratch storage for fixed-size algorithm records.
 *
 * All transfers use caller-owned bounded buffers. read/write transfers are
 * bounded by maxRecordsPerBatch; fill may cover a larger logical range but must
 * internally decompose it into bounded transfers. Implementations must reject
 * overflow, range, and buffer-size mismatches rather than truncating requests.
 */
class SIMPLNX_EXPORT ITemporaryRecordStore
{
public:
  virtual ~ITemporaryRecordStore() noexcept = default;

  /** @brief Returns the exact fixed record width in bytes. */
  virtual uint64 recordSize() const = 0;
  /** @brief Returns the current logical record count. */
  virtual uint64 recordCount() const = 0;
  /** @brief Returns the maximum record count accepted by one read or write transfer. */
  virtual uint64 maxRecordsPerBatch() const = 0;
  /** @brief Returns whether mutation requests are forbidden. */
  virtual bool isReadOnly() const = 0;
  /**
   * @brief Reads a bounded record range into caller-owned storage.
   * @param recordOffset Zero-based first record.
   * @param requestedRecordCount Maximum records requested, bounded by maxRecordsPerBatch().
   * @param records Output bytes large enough for the requested fixed-width records.
   * @param shouldCancel Cancellation flag checked by the implementation.
   * @return The number of records read, or a range, size, storage, or cancellation error.
   */
  virtual Result<uint64> read(uint64 recordOffset, uint64 requestedRecordCount, nonstd::span<std::byte> records, const std::atomic_bool& shouldCancel) const = 0;
  /**
   * @brief Writes a bounded range of complete fixed-width records.
   * @return A valid result or a read-only, range, size, storage, or cancellation error.
   */
  virtual Result<> write(uint64 recordOffset, uint64 recordCount, nonstd::span<const std::byte> records, const std::atomic_bool& shouldCancel) = 0;
  /**
   * @brief Repeats one complete record across a logical range using internally bounded transfers.
   * @return A valid result or a read-only, range, record-width, storage, or cancellation error.
   */
  virtual Result<> fill(uint64 recordOffset, uint64 recordCount, nonstd::span<const std::byte> record, const std::atomic_bool& shouldCancel) = 0;
  /**
   * @brief Changes the logical record count without changing record width.
   * @return A valid result or a read-only, capacity, storage, or cancellation error.
   */
  virtual Result<> resize(uint64 recordCount, const std::atomic_bool& shouldCancel) = 0;
};
} // namespace nx::core
