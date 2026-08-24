#pragma once

#include "simplnx/Common/Extent.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/Utilities/PositionalFileIO.hpp"
#include "simplnx/simplnx_export.hpp"

#include <H5Ipublic.h>
#include <nonstd/span.hpp>

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace nx::core::HDF5
{

/**
 * @class UnallocatedChunkError
 * @brief Thrown by @c ParallelChunkCodec::inflateChunk for a legitimately unallocated
 *        (never-written, fill-value) chunk of a sparse dataset.
 *
 * Such a chunk has no compressed bytes to inflate and is NOT an error at the chunk level:
 * a serial fill-aware read serves its fill values on demand. This distinct type lets a
 * best-effort caller (e.g. the cache-warm prewarm) catch and SKIP exactly this case while
 * still propagating a genuine inflate failure (short positional read, zlib error, size
 * mismatch) of an allocated chunk, which throws a plain @c std::runtime_error instead. The
 * "unallocated" determination rides the single metadata peek @c inflateChunk already does,
 * so no extra peek is needed to tell the two apart.
 */
class UnallocatedChunkError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

/**
 * @class ParallelChunkCodec
 * @brief Parallel raw-chunk inflater for deflate-compressed chunked datasets.
 *
 * @par Why this class exists
 * HDF5's gzip/deflate decompression runs *inside* @c H5Dread, which executes under a
 * process-wide, non-reentrant lock (@c nx::core::HDF5::Support::ApiLock() — the linked
 * HDF5 is not built thread-safe). Threading @c H5Dread therefore does NOT parallelize
 * inflation: every read serializes on that lock and the inflate, not the disk, dominates
 * a large multi-chunk read.
 *
 * @par What this class does instead
 * It reads raw compressed chunk bytes without running HDF5's filter pipeline, then inflates them
 * with @c zlib on worker threads. POSIX workers use a brief @c H5Dget_chunk_info_by_coord under
 * @c ApiLock() followed by lock-free @c pread through a shared descriptor. HDF5's Windows VFD
 * holds an exclusive whole-file range lock, so Windows workers use @c H5Dread_chunk under
 * @c ApiLock() for the raw transfer and still inflate off-lock. The dominant decompression work is
 * parallel on every platform.
 *
 * @par Removal trigger
 * This layer can be deleted, and the inflate replaced with HDF5's native equivalent, once
 * the linked HDF5 supports concurrent chunk reads/decompression.
 *
 * @par Eligibility (isEligible)
 * The parallel path applies only to a single-filter deflate pipeline AND a matching
 * host/file element byte order (or a single-byte element, where order is irrelevant) —
 * because the positional read bypasses the byte swap @c H5Dread would perform. Any other
 * pipeline (shuffle/szip/fletcher32/multi-filter/unknown) or a cross-endian multi-byte
 * dataset is ineligible; callers must fall back to a serial @c H5Dread for those.
 *
 * @par Output contract
 * The inflated bytes are byte-identical to what a serial, clamped @c H5Dread of the same
 * region produces: interior chunks return the inflated nominal (full, padded) buffer
 * directly; edge chunks have their in-bounds sub-region extracted. The result carries no
 * @c H5Dread type conversion (single-filter deflate datasets store data verbatim).
 *
 * @par Threading & lifetime
 * Construct on the calling thread (the eligibility probe runs once, in the ctor). The
 * dataset handle must remain valid for this object's lifetime. Parallel operations submit
 * disjoint chunk tasks to oneTBB's process-wide scheduler, reusing its worker pool across
 * calls; builds without multicore support execute the same tasks serially.
 */
class SIMPLNX_EXPORT ParallelChunkCodec
{
public:
  struct ChunkInfo
  {
    bool allocated = false;
    uint32 filterMask = 0;
    uint64 storedAddress = 0;
    uint64 storedSize = 0;
  };

  /**
   * @brief Constructs a codec bound to one open chunked dataset and computes eligibility.
   * @param filePath Backing HDF5 file path (used for the raw positional read).
   * @param datasetPath HDF5-internal dataset path (used in diagnostic messages).
   * @param tupleShape Dataset tuple dimensions (row-major).
   * @param chunkShape Chunk dimensions in tuple space (same rank as tupleShape).
   * @param componentShape Component dimensions (trailing dataset dims; never split across chunks).
   * @param elementSize sizeof(T) for the dataset element type.
   * @param datasetId Open HDF5 dataset id; must remain valid for this object's lifetime.
   */
  ParallelChunkCodec(std::filesystem::path filePath, std::string datasetPath, std::vector<uint64> tupleShape, std::vector<uint64> chunkShape, std::vector<uint64> componentShape, usize elementSize,
                     hid_t datasetId);

  ~ParallelChunkCodec();

  ParallelChunkCodec(const ParallelChunkCodec&) = delete;
  ParallelChunkCodec(ParallelChunkCodec&&) = delete;
  ParallelChunkCodec& operator=(const ParallelChunkCodec&) = delete;
  ParallelChunkCodec& operator=(ParallelChunkCodec&&) = delete;

  /**
   * @brief True when this dataset qualifies for the fast path: single-filter deflate
   *        pipeline AND (matching host/file byte order OR single-byte element). Computed
   *        once in the ctor via probeSingleDeflateEligibility.
   */
  bool isEligible() const;

  /**
   * @brief Inflates one chunk into a returned, edge-clamped buffer.
   *
   * A brief @c H5Dget_chunk_info_by_coord peek (under @c ApiLock()) yields the chunk's stored size
   * and filter mask. POSIX reads the raw bytes with lock-free positional I/O; Windows reads them
   * with @c H5Dread_chunk under @c ApiLock() because its HDF5 VFD locks the open file. Inflation
   * (or memcpy, if the chunk was stored uncompressed) remains off-lock. For interior chunks the
   * nominal buffer already IS the clamped layout; for edge chunks the in-bounds sub-region is
   * extracted so the result is byte-identical to a clamped serial @c H5Dread of that chunk region.
   *
   * That single peek also answers whether the chunk is allocated, so a best-effort caller
   * (e.g. a cache-warm prewarm over a window that may span never-written regions) can tell an
   * unallocated chunk from a real failure WITHOUT a separate allocation query: an unallocated
   * chunk throws @c UnallocatedChunkError (catch-and-skip), while a genuine inflate failure of
   * an allocated chunk throws a plain @c std::runtime_error (propagate).
   *
   * @param flatChunkIndex Flat logical chunk index (row-major over chunks-per-dimension).
   * @return Clamped chunk bytes, length = clampedTuples * numComponents * elementSize.
   * @throw UnallocatedChunkError if the chunk is not allocated (a sparse / fill-value region).
   * @throw std::runtime_error if the read is short or inflate fails on an allocated chunk.
   */
  std::vector<std::byte> inflateChunk(uint64 flatChunkIndex) const;

  /**
   * @brief Locates multiple chunks while holding the HDF5 API lock once for the batch.
   *
   * Returned metadata is position-aligned with @p flatChunkIndices. Callers may pass each value
   * to the metadata overload of @ref inflateChunk so the parallel worker phase performs no HDF5
   * metadata calls. This is a location snapshot: the dataset must remain structurally unchanged
   * until the corresponding inflates finish.
   */
  std::vector<ChunkInfo> getChunkInfos(nonstd::span<const uint64> flatChunkIndices) const;

  std::vector<std::byte> inflateChunk(uint64 flatChunkIndex, const ChunkInfo& chunkInfo) const;

  /**
   * @brief Inflates the given chunks in parallel directly into @p out.
   *
   * @p out is the full dataset laid out row-major over tupleShape ++ componentShape. Each
   * chunk is inflated (on a worker thread) and scattered into its natural offset in @p out,
   * producing the same bytes a serial @c H5Dread of the whole dataset would. Pass all flat
   * chunk indices for a whole-dataset read. Each chunk writes a disjoint region of @p out,
   * so no output lock is needed. Re-throws the first worker exception after joining all
   * workers.
   *
   * If a chunk is legitimately unallocated (a sparse / fill-value region), that chunk falls
   * back to a serial, clamped @c H5Dread under @c ApiLock() so the output stays correct for
   * fill-value datasets. A genuine error (short read, inflate failure) is propagated.
   *
   * @pre @p flatChunkIndices must contain DISTINCT indices. The lock-free, no-output-lock
   *      disjoint-write guarantee depends on it: two workers handed the same index would
   *      write the same region of @p out concurrently and race. The whole-dataset caller
   *      passes each chunk exactly once, so this is a precondition, not a runtime check.
   *
   * @param out Destination span sized for the full dataset (tupleShape ++ componentShape);
   *            throws if smaller than the required dataset byte size.
   * @param flatChunkIndices Distinct flat chunk indices to inflate into @p out.
   */
  void inflateChunksIntoSpan(nonstd::span<std::byte> out, nonstd::span<const uint64> flatChunkIndices) const;

  /**
   * @brief Compresses the given chunks in bounded parallel batches, then commits each batch with
   *        serial H5Dwrite_chunk calls — the write mirror of @ref inflateChunksIntoSpan.
   *
   * @p source holds a whole number of tuples laid out row-major over tupleShape ++ componentShape,
   * whose FIRST tuple is the global flat tuple @p sourceStartTuple. Band and full-array writes
   * share this one path: a full-array write passes every flat chunk index and @p sourceStartTuple
   * 0; a band passes a contiguous chunk range and that band's start tuple. Each chunk's bytes are
   * gathered from @p source into a nominal (full, padded) chunk buffer — the untouched tail of an
   * edge chunk is the padding — then prepared on a worker entirely OFF the HDF5 API lock. A
   * distributed trial-deflate stores clearly incompressible chunks verbatim with filter-mask bit 0
   * set; compressible chunks are zlib-compressed. Prepared chunks are retained only for a bounded
   * batch and committed by the calling thread; each leaf @c H5Dwrite_chunk takes
   * @c nx::core::HDF5::Support::ApiLock(). The written chunks read back byte-identically through a
   * serial @c H5Dread.
   *
   * Returns false on ANY failure — an ineligible dataset, a mis-sized or non-containing @p source,
   * an out-of-range index, or a zlib / @c H5Dwrite_chunk error — leaving the serial fallback to the
   * caller. This method NEVER throws.
   *
   * @pre @p flatChunkIndices must be DISTINCT. Each chunk's clamped tuple extent must lie fully
   *      inside [sourceStartTuple, sourceStartTuple + source tuples); range and containment are
   *      validated up front so bad arguments do not leave a partially written batch.
   *
   * @param source Tuple bytes to write (tupleShape ++ componentShape layout), starting at @p sourceStartTuple.
   * @param flatChunkIndices Distinct flat chunk indices to populate from @p source.
   * @param sourceStartTuple Global flat tuple index of @p source's first tuple (0 for a full-array write).
   * @param firstErrorOut Optional; when non-null and the call fails, receives the first failure's
   *        diagnostic (file, dataset, chunk index, zlib/HDF5 detail) for the caller's fallback log.
   * @return true if every requested chunk was adaptively stored; false on the first failure.
   */
  bool deflateSpanIntoChunks(nonstd::span<const std::byte> source, nonstd::span<const uint64> flatChunkIndices, uint64 sourceStartTuple = 0, std::string* firstErrorOut = nullptr) const;

  /**
   * @brief Compresses ONE nominal (full, padded) chunk buffer with zlib at the dataset's deflate
   *        level, entirely OFF the HDF5 API lock.
   *
   * @p nominalBytes must be the nominal chunk layout (chunkShape ++ componentShape, length =
   * nominalChunkElements * elementSize). This is the single-threaded entry point to the same
   * compression @ref deflateSpanIntoChunks runs on its worker threads, exposed for callers that
   * assemble one nominal buffer themselves (e.g. an edge-chunk re-pad or a compress-once fill).
   *
   * @param flatChunkIndex Flat logical chunk index (diagnostics only).
   * @param nominalBytes Nominal chunk bytes to compress.
   * @param errorOut Optional; receives the zlib diagnostic on failure.
   * @return The compressed bytes, or an empty vector on failure (zlib output is never empty for a
   *         non-empty input, so emptiness is an unambiguous failure signal).
   */
  std::vector<std::byte> compressNominalChunk(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* errorOut = nullptr) const;

  /**
   * @brief Writes pre-compressed bytes for ONE chunk via @c H5Dwrite_chunk under @c ApiLock().
   *
   * The full-rank chunk-origin offset is the tuple mins followed by component zeros (components are
   * never split across chunks); a filter mask of 0 records "all filters applied", so a later plain
   * @c H5Dread inflates the chunk normally. @c ApiLock() guards ONLY the leaf write. This is the
   * single-threaded entry point to the same write @ref deflateSpanIntoChunks runs on its workers.
   *
   * @param flatChunkIndex Flat logical chunk index.
   * @param compressedBytes Pre-compressed chunk bytes to store verbatim.
   * @param errorOut Optional; receives the HDF5 diagnostic on failure.
   * @return true on success; false if @c H5Dwrite_chunk failed.
   */
  bool writeCompressedChunk(uint64 flatChunkIndex, nonstd::span<const std::byte> compressedBytes, std::string* errorOut = nullptr) const;

  /**
   * @brief Adaptively stores one nominal chunk, applying deflate only when it shrinks the bytes.
   *
   * A distributed trial-deflate probe avoids full-chunk compression for clearly incompressible
   * data. Such a chunk is written verbatim with filter-mask bit 0 set, which is the standard
   * HDF5 representation for a skipped first filter. Compressible chunks retain the dataset's
   * configured deflate stream. The caller keeps ownership of @p nominalBytes until this synchronous
   * call returns.
   */
  bool writeNominalChunk(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* errorOut = nullptr) const;

  /**
   * @brief Prepares one adaptive representation and writes it to every full chunk in @p flatChunkIndices.
   *
   * This is the constant-fill/repeated-pattern mirror of @ref writeNominalChunk: the trial probe and,
   * when useful, full compression run exactly once. Every listed chunk must be an unclamped full chunk.
   */
  bool writeRepeatedNominalChunk(nonstd::span<const std::byte> nominalBytes, nonstd::span<const uint64> flatChunkIndices, std::string* errorOut = nullptr) const;

private:
  struct PreparedChunkBytes
  {
    std::vector<std::byte> filteredBytes;
    uint32 filterMask = 0;
  };

  /**
   * @brief Reads and inflates one already-located, allocated chunk into a clamped buffer.
   *
   * Shared inflate body for both @ref inflateChunk and the parallel worker. The caller has
   * already performed the @c H5Dget_chunk_info_by_coord peek (under @c ApiLock()) and passes the
   * resulting storage location. POSIX issues a lock-free positional read of the raw stored bytes;
   * Windows uses @c H5Dread_chunk under @c ApiLock() to respect its HDF5 whole-file lock. The method
   * then inflates (or, when @p filterMask marks deflate skipped, memcpy's) into the nominal chunk
   * buffer and extracts the in-bounds sub-region for an edge chunk. This single funnel is why the
   * parallel path peeks each allocated chunk exactly once.
   *
   * @param flatChunkIndex Flat logical chunk index (diagnostics only).
   * @param bounds Clamped tuple-space extent of the chunk.
   * @param storedAddress File offset of the chunk's raw stored bytes.
   * @param storedSize Number of raw stored bytes at @p storedAddress.
   * @param filterMask Chunk filter mask; bit 0 set means deflate was skipped (stored raw).
   * @return Clamped chunk bytes, length = clampedTuples * numComponents * elementSize.
   * @throw std::runtime_error if the read is short or inflate fails.
   */
  std::vector<std::byte> inflateChunkFromInfo(uint64 flatChunkIndex, const Extent& bounds, uint64 storedAddress, uint64 storedSize, uint32 filterMask) const;

  /**
   * @brief Gathers one chunk's bytes from @p source into a zero-initialized nominal chunk buffer.
   *
   * The returned buffer is the full, padded nominal chunk (chunkShape ++ componentShape, row-major):
   * for an edge chunk the untouched tail left by the zero initialization IS the padding. Bytes are
   * copied one contiguous innermost-dim stripe per in-bounds row, since consecutive innermost tuples
   * are contiguous in both @p source and the nominal chunk layout.
   *
   * @param flatChunkIndex Flat logical chunk index.
   * @param source Tuple bytes covering [sourceStartTuple, sourceStartTuple + source tuples).
   * @param sourceStartTuple Global flat tuple index of @p source's first tuple.
   * @return Nominal chunk bytes, length = m_NominalChunkElements * m_ElementSize.
   */
  std::vector<std::byte> gatherChunkBytes(uint64 flatChunkIndex, nonstd::span<const std::byte> source, uint64 sourceStartTuple) const;

  /**
   * @brief The single zlib-compression impl: compresses @p nominalBytes at the dataset's deflate
   *        level, entirely off the lock.
   *
   * Shared by @ref deflateSpanIntoChunks's workers (which pass the call's shared first-failure sink
   * + its mutex) and by the public @ref compressNominalChunk (which passes a private per-call sink +
   * mutex). One compression body, two entry points.
   *
   * @param flatChunkIndex Flat logical chunk index (diagnostics only).
   * @param nominalBytes Nominal chunk bytes to compress.
   * @param firstErrorOut Optional first-failure sink (shared across workers; written only if empty).
   * @param errorMutex Guards @p firstErrorOut against concurrent writes.
   * @return Compressed bytes, or an empty vector on a zlib failure (the detail is recorded).
   */
  std::vector<std::byte> compressChunkBytesImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* firstErrorOut, std::mutex& errorMutex) const;

  PreparedChunkBytes prepareChunkBytesImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* firstErrorOut, std::mutex& errorMutex) const;

  bool writeNominalChunkImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* firstErrorOut, std::mutex& errorMutex) const;

  /**
   * @brief The single chunk-write impl: writes already prepared bytes with @c H5Dwrite_chunk under
   *        @c ApiLock().
   *
   * The full-rank chunk-origin offset is the tuple mins followed by component zeros (components are
   * never split across chunks). @p filterMask records whether the bytes are filtered (0) or whether
   * deflate was skipped (bit 0 set). @c ApiLock() is held ONLY around the leaf write.
   *
   * @param flatChunkIndex Flat logical chunk index.
   * @param storedBytes Prepared chunk bytes to store verbatim.
   * @param filterMask HDF5 per-chunk filter mask for @p storedBytes.
   * @param firstErrorOut Optional first-failure sink (shared across workers; written only if empty).
   * @param errorMutex Guards @p firstErrorOut against concurrent writes.
   * @return true on success; false if @c H5Dwrite_chunk failed (the detail is recorded).
   */
  bool writeCompressedChunkImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> storedBytes, uint32 filterMask, std::string* firstErrorOut, std::mutex& errorMutex) const;

#ifndef _WIN32
  /**
   * @brief Returns the lazily opened POSIX descriptor shared by positional reads.
   *
   * @c pread does not mutate a shared file position, so POSIX workers can reuse one descriptor
   * without a per-chunk open/close cycle.
   */
  detail::FileHandle getPositionalReadHandle() const;
#endif

  std::filesystem::path m_FilePath;
  std::string m_DatasetPath;
  std::vector<uint64> m_TupleShape;
  std::vector<uint64> m_ChunkShape;     ///< Tuple-space chunk dims (rank == tupleShape).
  std::vector<uint64> m_ComponentShape; ///< Component dims (full chunk extent in these dims).
  usize m_ElementSize = 1;
  hid_t m_DatasetId = H5I_INVALID_HID;

  usize m_NumComponents = 1;        ///< product(componentShape).
  usize m_NominalChunkElements = 1; ///< product(chunkShape) * product(componentShape) — full padded chunk.
  int32 m_DeflateLevel = 1;         ///< Dataset's deflate level, captured by the eligibility probe; drives compress2.
  uint64 m_NumChunks = 1;           ///< getNumberOfChunks(tupleShape, chunkShape) — total chunk count for validation.
  bool m_Eligible = false;

#ifndef _WIN32
  // Serializes the exceptional short-read recovery only. Successful positional reads never
  // acquire this mutex, preserving the lock-free parallel inflate hot path.
  mutable std::mutex m_PositionalReadRecoveryMutex;
  mutable std::once_flag m_ReadHandleOpenOnce;
  mutable detail::FileHandle m_ReadFileHandle = detail::invalidFileHandle();
#endif
};

} // namespace nx::core::HDF5
