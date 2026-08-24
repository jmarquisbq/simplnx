#include "simplnx/Utilities/Parsing/HDF5/ParallelChunkCodec.hpp"

#include "simplnx/Utilities/Parsing/HDF5/ChunkIndex.hpp"
#include "simplnx/Utilities/Parsing/HDF5/DeflateEligibility.hpp"
#include "simplnx/Utilities/Parsing/HDF5/H5Support.hpp"
#include "simplnx/Utilities/Parsing/HDF5/ParallelChunkLoop.hpp"
#ifndef _WIN32
#include "simplnx/Utilities/PositionalFileIO.hpp"
#endif

#include <H5Dpublic.h>
#include <H5Fpublic.h>
#include <H5Ppublic.h>
#include <H5Spublic.h>

#include <fmt/core.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>

namespace nx::core::HDF5
{
namespace
{
constexpr usize k_IncompressibilityProbeBytes = 4 * 1024;
constexpr usize k_IncompressibilityProbeSegments = 4;
constexpr usize k_MaxPreparedBatchBytes = 64 * 1024 * 1024;
constexpr usize k_MaxPreparedBatchChunks = 64;

/**
 * @brief Reads one chunk's storage metadata via H5Dget_chunk_info_by_coord.
 *
 * @pre The caller holds @c Support::ApiLock(). A query failure or an unallocated chunk both yield
 * @c allocated == false; the caller distinguishes those cases by context (an unallocated chunk is
 * a valid fill-value region, not an error).
 */
ParallelChunkCodec::ChunkInfo peekChunkInfoAtOffsetUnlocked(hid_t datasetId, const hsize_t* offset)
{
  ParallelChunkCodec::ChunkInfo info;
  haddr_t address = HADDR_UNDEF;
  hsize_t storedSize = 0;
  const herr_t status = H5Dget_chunk_info_by_coord(datasetId, offset, &info.filterMask, &address, &storedSize);
  info.allocated = (status >= 0 && address != HADDR_UNDEF && storedSize != 0);
  info.storedAddress = static_cast<uint64>(address);
  info.storedSize = static_cast<uint64>(storedSize);
  return info;
}

ParallelChunkCodec::ChunkInfo peekChunkInfoUnlocked(hid_t datasetId, const Extent& bounds, usize componentRank)
{
  const usize tupleDims = bounds.min.size();
  const usize fullRank = tupleDims + componentRank;
  std::vector<hsize_t> offset(fullRank, 0);
  for(usize d = 0; d < tupleDims; ++d)
  {
    offset[d] = static_cast<hsize_t>(bounds.min[d]);
  }

  return peekChunkInfoAtOffsetUnlocked(datasetId, offset.data());
}

ParallelChunkCodec::ChunkInfo peekChunkInfo(hid_t datasetId, const Extent& bounds, usize componentRank)
{
  std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
  return peekChunkInfoUnlocked(datasetId, bounds, componentRank);
}

/// product of a shape vector (empty product is 1).
usize product(const std::vector<uint64>& shape)
{
  return std::accumulate(shape.begin(), shape.end(), usize{1}, std::multiplies<usize>());
}

/**
 * @brief Records the first failure into @p firstErrorOut, thread-safely (first-failure-wins).
 *
 * Workers run concurrently and any of them may fail; only the first message is kept so the
 * caller's fallback log shows the originating cause. A null sink is a no-op (the caller did not
 * ask for a diagnostic). @p errorMutex serializes the empty-check-then-write against other workers.
 */
void recordFirstError(std::string* firstErrorOut, std::mutex& errorMutex, std::string message)
{
  if(firstErrorOut == nullptr)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(errorMutex);
  if(firstErrorOut->empty())
  {
    *firstErrorOut = std::move(message);
  }
}

bool isLikelyIncompressible(nonstd::span<const std::byte> nominalBytes, int32 deflateLevel)
{
  if(nominalBytes.size() <= k_IncompressibilityProbeBytes)
  {
    return false;
  }

  constexpr usize k_SegmentBytes = k_IncompressibilityProbeBytes / k_IncompressibilityProbeSegments;
  std::array<std::byte, k_IncompressibilityProbeBytes> sample{};
  const usize maxSourceOffset = nominalBytes.size() - k_SegmentBytes;
  const usize sourceStride = maxSourceOffset / (k_IncompressibilityProbeSegments - 1);
  for(usize segment = 0; segment < k_IncompressibilityProbeSegments; ++segment)
  {
    const usize sourceOffset = segment + 1 == k_IncompressibilityProbeSegments ? maxSourceOffset : segment * sourceStride;
    std::memcpy(sample.data() + segment * k_SegmentBytes, nominalBytes.data() + sourceOffset, k_SegmentBytes);
  }

  const uLong sourceLength = static_cast<uLong>(sample.size());
  uLongf compressedLength = compressBound(sourceLength);
  std::vector<std::byte> compressed(static_cast<usize>(compressedLength));
  const int result = compress2(reinterpret_cast<Bytef*>(compressed.data()), &compressedLength, reinterpret_cast<const Bytef*>(sample.data()), sourceLength, deflateLevel);
  return result == Z_OK && compressedLength >= sourceLength;
}
} // namespace

ParallelChunkCodec::ParallelChunkCodec(std::filesystem::path filePath, std::string datasetPath, std::vector<uint64> tupleShape, std::vector<uint64> chunkShape, std::vector<uint64> componentShape,
                                       usize elementSize, hid_t datasetId)
: m_FilePath(std::move(filePath))
, m_DatasetPath(std::move(datasetPath))
, m_TupleShape(std::move(tupleShape))
, m_ChunkShape(std::move(chunkShape))
, m_ComponentShape(std::move(componentShape))
, m_ElementSize(elementSize)
, m_DatasetId(datasetId)
{
  m_NumComponents = product(m_ComponentShape);
  m_NominalChunkElements = product(m_ChunkShape) * m_NumComponents;
  m_NumChunks = getNumberOfChunks(m_TupleShape, m_ChunkShape);
  // Single probe (single-filter deflate + byte-order gate). The captured deflate level makes the
  // deflate path's compress2 produce exactly what the dataset's creation property list specifies;
  // the inflate side ignores it (it inflates with whatever level the chunks were compressed at).
  m_Eligible = probeSingleDeflateEligibility(m_DatasetId, m_ElementSize, &m_DeflateLevel);
}

ParallelChunkCodec::~ParallelChunkCodec()
{
#ifndef _WIN32
  if(detail::isValidFileHandle(m_ReadFileHandle))
  {
    detail::closeFileHandle(m_ReadFileHandle);
  }
#endif
}

#ifndef _WIN32
detail::FileHandle ParallelChunkCodec::getPositionalReadHandle() const
{
  std::call_once(m_ReadHandleOpenOnce, [this]() {
    m_ReadFileHandle = detail::openFileForRead(m_FilePath.string());
    if(!detail::isValidFileHandle(m_ReadFileHandle))
    {
      throw std::runtime_error(fmt::format("ParallelChunkCodec: failed to open '{}'", m_FilePath.string()));
    }
  });
  return m_ReadFileHandle;
}
#endif

bool ParallelChunkCodec::isEligible() const
{
  return m_Eligible;
}

std::vector<std::byte> ParallelChunkCodec::inflateChunk(uint64 flatChunkIndex) const
{
  const Extent bounds = getChunkBounds(flatChunkIndex, m_TupleShape, m_ChunkShape);

  // Metadata: chunk file address, stored (compressed) size, filter mask. Peeked once here;
  // the inflate body below reuses it without a second metadata call. This is the ONLY
  // metadata peek on the read path, so a best-effort prewarm caller pays exactly one peek
  // per cache miss and none on a cache hit (its loader never runs).
  const ChunkInfo info = peekChunkInfo(m_DatasetId, bounds, m_ComponentShape.size());
  if(!info.allocated)
  {
    // An unallocated chunk is a legitimate sparse / fill-value region, not a failure: signal
    // it with the distinct UnallocatedChunkError so a prewarm caller catches and skips it,
    // while a genuine inflate failure below throws a plain std::runtime_error and propagates.
    throw UnallocatedChunkError(fmt::format("ParallelChunkCodec: chunk {} not allocated (sparse / fill-value region) in '{}:{}'", flatChunkIndex, m_FilePath.string(), m_DatasetPath));
  }
  return inflateChunk(flatChunkIndex, info);
}

std::vector<ParallelChunkCodec::ChunkInfo> ParallelChunkCodec::getChunkInfos(nonstd::span<const uint64> flatChunkIndices) const
{
  std::vector<ChunkInfo> chunkInfos;
  chunkInfos.reserve(flatChunkIndices.size());
  const std::vector<uint64> chunksPerDimension = getChunksPerDimension(m_TupleShape, m_ChunkShape);
  std::vector<hsize_t> offset(m_TupleShape.size() + m_ComponentShape.size(), 0);
  std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
  for(uint64 flatChunkIndex : flatChunkIndices)
  {
    uint64 remaining = flatChunkIndex;
    for(usize reverseDimension = m_TupleShape.size(); reverseDimension > 0; --reverseDimension)
    {
      const usize dimension = reverseDimension - 1;
      const uint64 chunkCoordinate = remaining % chunksPerDimension[dimension];
      remaining /= chunksPerDimension[dimension];
      offset[dimension] = static_cast<hsize_t>(chunkCoordinate * m_ChunkShape[dimension]);
    }
    chunkInfos.push_back(peekChunkInfoAtOffsetUnlocked(m_DatasetId, offset.data()));
  }
  return chunkInfos;
}

std::vector<std::byte> ParallelChunkCodec::inflateChunk(uint64 flatChunkIndex, const ChunkInfo& chunkInfo) const
{
  if(!chunkInfo.allocated)
  {
    throw UnallocatedChunkError(fmt::format("ParallelChunkCodec: chunk {} not allocated (sparse / fill-value region) in '{}:{}'", flatChunkIndex, m_FilePath.string(), m_DatasetPath));
  }
  const Extent bounds = getChunkBounds(flatChunkIndex, m_TupleShape, m_ChunkShape);
  return inflateChunkFromInfo(flatChunkIndex, bounds, chunkInfo.storedAddress, chunkInfo.storedSize, chunkInfo.filterMask);
}

std::vector<std::byte> ParallelChunkCodec::inflateChunkFromInfo(uint64 flatChunkIndex, const Extent& bounds, uint64 storedAddress, uint64 storedSize, uint32 filterMask) const
{
  const usize tupleDims = m_TupleShape.size();

  std::vector<std::byte> stored(static_cast<usize>(storedSize));
#ifdef _WIN32
  // HDF5's Windows VFD holds an exclusive LockFileEx range over the entire open file, so an
  // independent native handle cannot read raw chunk ranges while the dataset is open. Read only
  // the stored chunk through HDF5 under ApiLock, then release the lock before doing the expensive
  // inflate on this worker.
  const usize fullRank = tupleDims + m_ComponentShape.size();
  std::vector<hsize_t> offset(fullRank, 0);
  for(usize dimension = 0; dimension < tupleDims; ++dimension)
  {
    offset[dimension] = static_cast<hsize_t>(bounds.min[dimension]);
  }
  uint32 readFilterMask = 0;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    if(H5Dread_chunk(m_DatasetId, H5P_DEFAULT, offset.data(), &readFilterMask, stored.data()) < 0)
    {
      throw std::runtime_error(fmt::format("ParallelChunkCodec: H5Dread_chunk failed on chunk {} of '{}:{}'", flatChunkIndex, m_FilePath.string(), m_DatasetPath));
    }
  }
  filterMask = readFilterMask;
  static_cast<void>(storedAddress);
#else
  // Lock-free positional read of the raw stored bytes. POSIX pread workers reuse one lazily opened
  // descriptor, and retry after an HDF5 flush only when an independently opened descriptor cannot
  // yet see a recently extended file.
  auto readStoredBytes = [&]() -> std::ptrdiff_t {
    const nx::core::detail::FileHandle rawHandle = getPositionalReadHandle();
    return nx::core::detail::positionalRead(rawHandle, stored.data(), static_cast<std::size_t>(storedSize), static_cast<uint64_t>(storedAddress));
  };

  std::ptrdiff_t got = readStoredBytes();
  if(got != static_cast<std::ptrdiff_t>(storedSize))
  {
    // Keep the normal read path lock- and flush-free: only a short read enters recovery. The second
    // read under the recovery mutex avoids redundant H5Fflush calls when another worker already
    // recovered the file before this worker acquired the mutex.
    std::lock_guard<std::mutex> recoveryLock(m_PositionalReadRecoveryMutex);
    got = readStoredBytes();
    if(got != static_cast<std::ptrdiff_t>(storedSize))
    {
      {
        std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
        if(H5Fflush(m_DatasetId, H5F_SCOPE_LOCAL) < 0)
        {
          throw std::runtime_error(
              fmt::format("ParallelChunkCodec: H5Fflush failed while recovering a short positional read on chunk {} of '{}:{}'", flatChunkIndex, m_FilePath.string(), m_DatasetPath));
        }
      }
      got = readStoredBytes();
    }
  }
  if(got != static_cast<std::ptrdiff_t>(storedSize))
  {
    throw std::runtime_error(fmt::format("ParallelChunkCodec: short positional read on chunk {} of '{}' ({} of {} bytes)", flatChunkIndex, m_FilePath.string(), got, storedSize));
  }
#endif

  // Inflate (or memcpy) into the nominal (full, padded) chunk buffer.
  const usize nominalBytes = m_NominalChunkElements * m_ElementSize;
  std::vector<std::byte> nominal(nominalBytes);

  // filter_mask bit i is SET when filter i was SKIPPED for this chunk. Eligibility
  // guarantees deflate is the single filter at pipeline index 0, so bit 0 set means HDF5
  // stored this chunk uncompressed (deflate didn't shrink it) -> memcpy, not inflate.
  const bool deflateSkipped = (filterMask & 0x1u) != 0u;
  if(deflateSkipped)
  {
    if(static_cast<usize>(storedSize) != nominalBytes)
    {
      throw std::runtime_error(fmt::format("ParallelChunkCodec: uncompressed chunk {} size {} != nominal {} in '{}'", flatChunkIndex, storedSize, nominalBytes, m_FilePath.string()));
    }
    std::memcpy(nominal.data(), stored.data(), nominalBytes);
  }
  else
  {
    // zlib's uLong/uLongf are 32-bit on LLP64 (Windows), so a nominal chunk >= 4 GiB would
    // silently truncate in the uncompress() length arguments. Reject it explicitly.
    if(nominalBytes > static_cast<usize>(std::numeric_limits<uLongf>::max()))
    {
      throw std::runtime_error(
          fmt::format("ParallelChunkCodec: nominal chunk {} size {} exceeds zlib's {}-byte limit in '{}'", flatChunkIndex, nominalBytes, std::numeric_limits<uLongf>::max(), m_FilePath.string()));
    }
    uLongf destLen = static_cast<uLongf>(nominalBytes);
    const int zret = uncompress(reinterpret_cast<Bytef*>(nominal.data()), &destLen, reinterpret_cast<const Bytef*>(stored.data()), static_cast<uLong>(storedSize));
    if(zret != Z_OK || destLen != static_cast<uLongf>(nominalBytes))
    {
      throw std::runtime_error(
          fmt::format("ParallelChunkCodec: zlib uncompress failed (ret={}, got {} of {} bytes) on chunk {} of '{}'", zret, destLen, nominalBytes, flatChunkIndex, m_FilePath.string()));
    }
  }
  // Interior chunk: nominal buffer already IS the clamped layout (row-major over
  // chunkShape ++ componentShape). Edge chunk: extract the in-bounds sub-region so the
  // result is byte-identical to a clamped serial H5Dread of that chunk region.
  bool isInterior = true;
  std::vector<uint64> clampedTupleDims(tupleDims);
  for(usize d = 0; d < tupleDims; ++d)
  {
    clampedTupleDims[d] = bounds.max[d] - bounds.min[d] + 1;
    if(clampedTupleDims[d] != m_ChunkShape[d])
    {
      isInterior = false;
    }
  }
  if(isInterior)
  {
    return nominal;
  }

  const usize clampedTuples = product(clampedTupleDims);
  const usize compBytes = m_NumComponents * m_ElementSize;
  std::vector<std::byte> clamped(clampedTuples * compBytes);
  for(usize flatClamped = 0; flatClamped < clampedTuples; ++flatClamped)
  {
    // Chunk-local N-D tuple position (0-based, < clampedTupleDims <= chunkShape).
    const std::vector<uint64> nd = flatToNd(static_cast<uint64>(flatClamped), clampedTupleDims);
    const usize nominalTupleFlat = static_cast<usize>(ndToFlat(nd, m_ChunkShape));
    std::memcpy(clamped.data() + flatClamped * compBytes, nominal.data() + nominalTupleFlat * compBytes, compBytes);
  }
  return clamped;
}

void ParallelChunkCodec::inflateChunksIntoSpan(nonstd::span<std::byte> out, nonstd::span<const uint64> flatChunkIndices) const
{
  const usize chunkCount = flatChunkIndices.size();
  if(chunkCount == 0)
  {
    return;
  }

  const usize tupleDims = m_TupleShape.size();
  const usize compBytes = m_NumComponents * m_ElementSize;

  // Validate the destination size before any worker writes: the scatter computes a global
  // flat offset per tuple and memcpy's into out.data() + offset * compBytes with no per-write
  // bounds check, so an undersized span would be a silent out-of-bounds heap write. This cold,
  // single-threaded check converts that into a clear, fail-fast error.
  const usize requiredBytes = product(m_TupleShape) * compBytes;
  if(out.size() < requiredBytes)
  {
    throw std::runtime_error(fmt::format("ParallelChunkCodec: output span too small for '{}:{}' ({} < {} bytes)", m_FilePath.string(), m_DatasetPath, out.size(), requiredBytes));
  }

  // Scatters one chunk's clamped, in-bounds bytes into their natural offsets in @p out.
  // The clamped buffer is row-major over clampedTupleDims ++ componentShape; out is
  // row-major over m_TupleShape ++ m_ComponentShape. Each chunk writes a disjoint region.
  //
  // Rather than a per-tuple copy (one memcpy of compBytes for every tuple, which on a
  // full-slab chunk degenerates into hundreds of millions of tiny copies plus N-D coordinate
  // math), this collapses the largest run of tuples that is contiguous in BOTH layouts and
  // copies it in a single memcpy. This mirrors what AbstractDataStore::copyFromBuffer does for
  // a contiguous flat range, so the cost is bulk-transfer (memory-bandwidth) bound, not
  // per-element bound — for an interior full-slab chunk the whole chunk is one contiguous run.
  auto scatter = [&](const Extent& bounds, const std::vector<std::byte>& clamped) {
    std::vector<uint64> clampedTupleDims(tupleDims);
    for(usize d = 0; d < tupleDims; ++d)
    {
      clampedTupleDims[d] = bounds.max[d] - bounds.min[d] + 1;
    }

    // The innermost tuple dimension is always contiguous in row-major order. Merge each
    // next-outer dimension into the run only while the dimension just inside it is fully
    // spanned (clamped extent == full extent): a partially-spanned inner dimension leaves a
    // gap in @p out between successive outer steps, breaking contiguity. firstRunDim is the
    // outermost dimension folded into the contiguous run; dims [0, firstRunDim) are iterated.
    usize runTuples = clampedTupleDims[tupleDims - 1];
    usize firstRunDim = tupleDims - 1;
    for(usize d = tupleDims - 1; d-- > 0;)
    {
      if(clampedTupleDims[d + 1] != m_TupleShape[d + 1])
      {
        break;
      }
      runTuples *= clampedTupleDims[d];
      firstRunDim = d;
    }

    const usize runBytes = runTuples * compBytes;
    const std::vector<uint64> outerDims(clampedTupleDims.begin(), clampedTupleDims.begin() + firstRunDim);
    usize numRuns = 1;
    for(usize d = 0; d < firstRunDim; ++d)
    {
      numRuns *= clampedTupleDims[d];
    }

    // Inner (merged) dimensions start at their chunk minimum and never change across runs.
    std::vector<uint64> globalCoords(tupleDims);
    for(usize d = firstRunDim; d < tupleDims; ++d)
    {
      globalCoords[d] = bounds.min[d];
    }
    for(usize runIdx = 0; runIdx < numRuns; ++runIdx)
    {
      if(firstRunDim > 0)
      {
        const std::vector<uint64> outerNd = flatToNd(static_cast<uint64>(runIdx), outerDims);
        for(usize d = 0; d < firstRunDim; ++d)
        {
          globalCoords[d] = bounds.min[d] + outerNd[d];
        }
      }
      const usize globalFlat = static_cast<usize>(ndToFlat(globalCoords, m_TupleShape));
      std::memcpy(out.data() + globalFlat * compBytes, clamped.data() + runIdx * runBytes, runBytes);
    }
  };

  // Serial, fill-aware read for a legitimately unallocated (sparse / fill-value) chunk:
  // a clamped H5Dread of the chunk region into a returned clamped buffer (the loader hands
  // it to the sink, which scatters it exactly like an inflated chunk). Returning the fill
  // bytes rather than throwing UnallocatedChunkError keeps the output correct for fill-value
  // datasets — the engine would SKIP a throwing chunk, leaving its region unwritten.
  // ApiLock() guards ONLY the leaf HDF5 calls (this is a leaf read path). In practice
  // simplnx writes full arrays, so every chunk is allocated and this path is rarely taken;
  // it is the documented correctness backstop for fill-value datasets.
  auto serialFillRead = [&](uint64 flatChunkIndex, const Extent& bounds) -> std::vector<std::byte> {
    std::vector<uint64> clampedTupleDims(tupleDims);
    for(usize d = 0; d < tupleDims; ++d)
    {
      clampedTupleDims[d] = bounds.max[d] - bounds.min[d] + 1;
    }
    const usize clampedTuples = product(clampedTupleDims);
    std::vector<std::byte> clamped(clampedTuples * compBytes);

    const usize fullRank = tupleDims + m_ComponentShape.size();
    std::vector<hsize_t> fileStart(fullRank, 0);
    std::vector<hsize_t> count(fullRank, 0);
    std::vector<hsize_t> memDims(fullRank, 0);
    for(usize d = 0; d < tupleDims; ++d)
    {
      fileStart[d] = static_cast<hsize_t>(bounds.min[d]);
      count[d] = static_cast<hsize_t>(clampedTupleDims[d]);
      memDims[d] = static_cast<hsize_t>(clampedTupleDims[d]);
    }
    for(usize d = 0; d < m_ComponentShape.size(); ++d)
    {
      fileStart[tupleDims + d] = 0;
      count[tupleDims + d] = static_cast<hsize_t>(m_ComponentShape[d]);
      memDims[tupleDims + d] = static_cast<hsize_t>(m_ComponentShape[d]);
    }

    {
      std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
      const hid_t fileSpace = H5Dget_space(m_DatasetId);
      const hid_t memSpace = H5Screate_simple(static_cast<int>(fullRank), memDims.data(), nullptr);
      const hid_t dtype = H5Dget_type(m_DatasetId);
      const herr_t selStatus = (fileSpace < 0) ? static_cast<herr_t>(-1) : H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, fileStart.data(), nullptr, count.data(), nullptr);
      const herr_t readStatus = (fileSpace < 0 || memSpace < 0 || dtype < 0 || selStatus < 0) ? static_cast<herr_t>(-1) : H5Dread(m_DatasetId, dtype, memSpace, fileSpace, H5P_DEFAULT, clamped.data());
      if(dtype >= 0)
      {
        H5Tclose(dtype);
      }
      if(memSpace >= 0)
      {
        H5Sclose(memSpace);
      }
      if(fileSpace >= 0)
      {
        H5Sclose(fileSpace);
      }
      // Name the first failing call so the diagnostic pinpoints the cause instead of only
      // surfacing a generic "serial H5Dread failed".
      const char* failedCall = nullptr;
      if(fileSpace < 0)
      {
        failedCall = "H5Dget_space";
      }
      else if(memSpace < 0)
      {
        failedCall = "H5Screate_simple";
      }
      else if(dtype < 0)
      {
        failedCall = "H5Dget_type";
      }
      else if(selStatus < 0)
      {
        failedCall = "H5Sselect_hyperslab";
      }
      else if(readStatus < 0)
      {
        failedCall = "H5Dread";
      }
      if(failedCall != nullptr)
      {
        throw std::runtime_error(fmt::format("ParallelChunkCodec: serial fill-value read failed at {} on chunk {} of '{}:{}'", failedCall, flatChunkIndex, m_FilePath.string(), m_DatasetPath));
      }
    }
    return clamped;
  };

  // Loads one chunk's clamped bytes: peek allocation ONCE, then either inflate the allocated
  // chunk from the info just fetched (no second metadata call under the lock) or serve the
  // unallocated (fill-value) chunk's bytes via the serial fill-aware read. Both branches RETURN
  // bytes for the sink to scatter — a fill-value chunk is served, not skipped, so its region of
  // @p out is written. Only a genuine failure (short read / inflate error) of an allocated chunk
  // throws, which the engine records and rethrows after the scheduled batch finishes.
  auto loader = [&](uint64 idx) -> std::vector<std::byte> {
    const Extent bounds = getChunkBounds(idx, m_TupleShape, m_ChunkShape);
    const ChunkInfo info = peekChunkInfo(m_DatasetId, bounds, m_ComponentShape.size());
    if(info.allocated)
    {
      return inflateChunkFromInfo(idx, bounds, info.storedAddress, info.storedSize, info.filterMask);
    }
    return serialFillRead(idx, bounds);
  };

  // Scatters the loaded bytes into their disjoint region of @p out. Tasks own disjoint local
  // indices and each chunk writes a disjoint output region, so no lock
  // is needed (the distinct-index precondition guarantees it). Bounds are recomputed from the
  // chunk index exactly as in the loader.
  auto sink = [&](usize localIndex, std::vector<std::byte>&& bytes) {
    const Extent bounds = getChunkBounds(flatChunkIndices[localIndex], m_TupleShape, m_ChunkShape);
    scatter(bounds, bytes);
  };

  // Fan the peek/inflate/fill loader and the scatter sink across the process-wide task scheduler
  // via the shared engine, which owns the disjoint-write guarantee and the record-first-then-
  // rethrow error policy. A genuine failure surfaces after the batch finishes.
  ParallelLoadChunks<std::vector<std::byte>>(flatChunkIndices, loader, sink);
}

std::vector<std::byte> ParallelChunkCodec::gatherChunkBytes(uint64 flatChunkIndex, nonstd::span<const std::byte> source, uint64 sourceStartTuple) const
{
  const usize tupleDims = m_TupleShape.size();
  const Extent bounds = getChunkBounds(flatChunkIndex, m_TupleShape, m_ChunkShape);

  std::vector<uint64> clampedTupleDims(tupleDims);
  for(usize d = 0; d < tupleDims; ++d)
  {
    clampedTupleDims[d] = bounds.max[d] - bounds.min[d] + 1;
  }

  // Zero-initialized nominal buffer: the untouched tail of an edge chunk IS the padding, so a later
  // serial H5Dread of the (clamped) in-bounds region sees exactly the gathered source bytes.
  const usize compBytes = m_NumComponents * m_ElementSize;
  std::vector<std::byte> nominal(m_NominalChunkElements * m_ElementSize);

  // Collapse the largest run of tuples that is contiguous in BOTH the dataset source (row-major
  // over m_TupleShape) and the nominal chunk buffer (row-major over m_ChunkShape), and copy it in
  // one memcpy. The innermost tuple dimension is always contiguous; each next-outer dimension can
  // be merged only while the dimension just inside it fully spans BOTH layouts (clamped extent ==
  // source extent AND == chunk extent) — a partial inner dimension leaves a gap in one layout and
  // breaks contiguity. Without this collapse a trailing size-1 component dimension would degrade
  // the copy to one tuple at a time. firstRunDim is the outermost dimension folded into the run;
  // dims [0, firstRunDim) are iterated, one memcpy each.
  usize runTuples = clampedTupleDims[tupleDims - 1];
  usize firstRunDim = tupleDims - 1;
  for(usize d = tupleDims - 1; d-- > 0;)
  {
    if(clampedTupleDims[d + 1] != m_TupleShape[d + 1] || clampedTupleDims[d + 1] != m_ChunkShape[d + 1])
    {
      break;
    }
    runTuples *= clampedTupleDims[d];
    firstRunDim = d;
  }

  const usize runBytes = runTuples * compBytes;
  const std::vector<uint64> outerDims(clampedTupleDims.begin(), clampedTupleDims.begin() + firstRunDim);
  uint64 numRuns = 1;
  for(usize d = 0; d < firstRunDim; ++d)
  {
    numRuns *= clampedTupleDims[d];
  }

  // Inner (merged) dimensions start at the chunk's local origin (0) and its global minimum, and
  // never change across runs; only the iterated outer dimensions advance.
  std::vector<uint64> localNd(tupleDims, 0);
  std::vector<uint64> globalNd(tupleDims, 0);
  for(usize d = firstRunDim; d < tupleDims; ++d)
  {
    globalNd[d] = bounds.min[d];
  }
  for(uint64 runIdx = 0; runIdx < numRuns; ++runIdx)
  {
    if(firstRunDim > 0)
    {
      const std::vector<uint64> outerNd = flatToNd(runIdx, outerDims);
      for(usize d = 0; d < firstRunDim; ++d)
      {
        localNd[d] = outerNd[d];
        globalNd[d] = bounds.min[d] + outerNd[d];
      }
    }
    // The source begins at sourceStartTuple, so subtract it to land at the run's offset in source.
    const usize srcOffset = static_cast<usize>(ndToFlat(globalNd, m_TupleShape) - sourceStartTuple) * compBytes;
    const usize dstOffset = static_cast<usize>(ndToFlat(localNd, m_ChunkShape)) * compBytes;
    std::memcpy(nominal.data() + dstOffset, source.data() + srcOffset, runBytes);
  }
  return nominal;
}

std::vector<std::byte> ParallelChunkCodec::compressChunkBytesImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* firstErrorOut, std::mutex& errorMutex) const
{
  // Compress OFF the HDF5 API lock: this is the dominant cost and the whole point of the parallel
  // path. zlib's uLong is 32-bit on LLP64 (Windows), so a nominal chunk >= 4 GiB would silently
  // truncate the length argument; reject it explicitly rather than corrupt the stream.
  if(nominalBytes.size() > static_cast<usize>(std::numeric_limits<uLong>::max()))
  {
    recordFirstError(firstErrorOut, errorMutex,
                     fmt::format("ParallelChunkCodec: nominal chunk {} size {} exceeds zlib's {}-byte limit in '{}:{}'", flatChunkIndex, nominalBytes.size(), std::numeric_limits<uLong>::max(),
                                 m_FilePath.string(), m_DatasetPath));
    return {};
  }
  const uLong srcLen = static_cast<uLong>(nominalBytes.size());
  uLongf destLen = compressBound(srcLen);
  // compress2 overwrites the destination and usually produces far fewer bytes than
  // compressBound. Avoid value-initializing the full bound-sized allocation, then copy
  // only the actual compressed prefix into the returned vector.
  std::unique_ptr<std::byte[]> compressed = std::unique_ptr<std::byte[]>(new std::byte[static_cast<usize>(destLen)]);
  const int zret = compress2(reinterpret_cast<Bytef*>(compressed.get()), &destLen, reinterpret_cast<const Bytef*>(nominalBytes.data()), srcLen, m_DeflateLevel);
  if(zret != Z_OK)
  {
    recordFirstError(firstErrorOut, errorMutex,
                     fmt::format("ParallelChunkCodec: zlib compress2 failed (ret={}, level={}) on chunk {} of '{}:{}'", zret, m_DeflateLevel, flatChunkIndex, m_FilePath.string(), m_DatasetPath));
    return {};
  }
  return std::vector<std::byte>(compressed.get(), compressed.get() + static_cast<usize>(destLen));
}

ParallelChunkCodec::PreparedChunkBytes ParallelChunkCodec::prepareChunkBytesImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* firstErrorOut,
                                                                                 std::mutex& errorMutex) const
{
  if(isLikelyIncompressible(nominalBytes, m_DeflateLevel))
  {
    return {{}, 0x1U};
  }

  std::vector<std::byte> compressed = compressChunkBytesImpl(flatChunkIndex, nominalBytes, firstErrorOut, errorMutex);
  if(compressed.empty())
  {
    return {};
  }
  if(compressed.size() >= nominalBytes.size())
  {
    return {{}, 0x1U};
  }
  return {std::move(compressed), 0};
}

bool ParallelChunkCodec::writeCompressedChunkImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> storedBytes, uint32 filterMask, std::string* firstErrorOut, std::mutex& errorMutex) const
{
  // Full-rank chunk-origin offset: tuple mins ++ component zeros (components never split).
  const usize tupleDims = m_TupleShape.size();
  const usize fullRank = tupleDims + m_ComponentShape.size();
  const Extent bounds = getChunkBounds(flatChunkIndex, m_TupleShape, m_ChunkShape);
  std::vector<hsize_t> offset(fullRank, 0);
  for(usize d = 0; d < tupleDims; ++d)
  {
    offset[d] = static_cast<hsize_t>(bounds.min[d]);
  }

  // filter_mask bit 0 records whether deflate was skipped for this chunk. ApiLock() guards ONLY
  // this leaf write — never the off-lock gather/compress or incompressibility probe above it.
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    if(H5Dwrite_chunk(m_DatasetId, H5P_DEFAULT, filterMask, offset.data(), storedBytes.size(), storedBytes.data()) >= 0)
    {
      return true;
    }
  }
  recordFirstError(firstErrorOut, errorMutex,
                   fmt::format("ParallelChunkCodec: H5Dwrite_chunk failed ({} stored bytes, filter mask {}) on chunk {} of '{}:{}'", storedBytes.size(), filterMask, flatChunkIndex,
                               m_FilePath.string(), m_DatasetPath));
  return false;
}

bool ParallelChunkCodec::writeNominalChunkImpl(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* firstErrorOut, std::mutex& errorMutex) const
{
  const PreparedChunkBytes prepared = prepareChunkBytesImpl(flatChunkIndex, nominalBytes, firstErrorOut, errorMutex);
  if(prepared.filterMask == 0 && prepared.filteredBytes.empty())
  {
    return false;
  }
  const nonstd::span<const std::byte> storedBytes = prepared.filterMask == 0 ? nonstd::span<const std::byte>(prepared.filteredBytes.data(), prepared.filteredBytes.size()) : nominalBytes;
  return writeCompressedChunkImpl(flatChunkIndex, storedBytes, prepared.filterMask, firstErrorOut, errorMutex);
}

std::vector<std::byte> ParallelChunkCodec::compressNominalChunk(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* errorOut) const
{
  // Single-threaded wrapper over the shared compression impl: a private per-call mutex stands in
  // for the worker-shared one, so the impl's first-failure-wins recording writes straight into the
  // caller's errorOut. No worker contention exists here, but the impl signature is uniform.
  if(errorOut != nullptr)
  {
    errorOut->clear();
  }
  std::mutex errorMutex;
  return compressChunkBytesImpl(flatChunkIndex, nominalBytes, errorOut, errorMutex);
}

bool ParallelChunkCodec::writeCompressedChunk(uint64 flatChunkIndex, nonstd::span<const std::byte> compressedBytes, std::string* errorOut) const
{
  // Single-threaded wrapper over the shared write impl (see compressNominalChunk for the mutex note).
  if(errorOut != nullptr)
  {
    errorOut->clear();
  }
  std::mutex errorMutex;
  return writeCompressedChunkImpl(flatChunkIndex, compressedBytes, 0, errorOut, errorMutex);
}

bool ParallelChunkCodec::writeNominalChunk(uint64 flatChunkIndex, nonstd::span<const std::byte> nominalBytes, std::string* errorOut) const
{
  if(errorOut != nullptr)
  {
    errorOut->clear();
  }
  std::mutex errorMutex;
  if(!m_Eligible || flatChunkIndex >= m_NumChunks || nominalBytes.size() != m_NominalChunkElements * m_ElementSize)
  {
    recordFirstError(errorOut, errorMutex,
                     fmt::format("ParallelChunkCodec: invalid nominal-chunk write for chunk {} of '{}:{}' (eligible={}, bytes={}, expected={}, chunks={})", flatChunkIndex, m_FilePath.string(),
                                 m_DatasetPath, m_Eligible, nominalBytes.size(), m_NominalChunkElements * m_ElementSize, m_NumChunks));
    return false;
  }
  return writeNominalChunkImpl(flatChunkIndex, nominalBytes, errorOut, errorMutex);
}

bool ParallelChunkCodec::writeRepeatedNominalChunk(nonstd::span<const std::byte> nominalBytes, nonstd::span<const uint64> flatChunkIndices, std::string* errorOut) const
{
  if(errorOut != nullptr)
  {
    errorOut->clear();
  }
  std::mutex errorMutex;
  if(!m_Eligible || nominalBytes.size() != m_NominalChunkElements * m_ElementSize)
  {
    recordFirstError(errorOut, errorMutex,
                     fmt::format("ParallelChunkCodec: invalid repeated nominal-chunk write for '{}:{}' (eligible={}, bytes={}, expected={})", m_FilePath.string(), m_DatasetPath, m_Eligible,
                                 nominalBytes.size(), m_NominalChunkElements * m_ElementSize));
    return false;
  }
  if(flatChunkIndices.empty())
  {
    return true;
  }
  for(uint64 flatChunkIndex : flatChunkIndices)
  {
    if(flatChunkIndex >= m_NumChunks)
    {
      recordFirstError(errorOut, errorMutex,
                       fmt::format("ParallelChunkCodec: repeated nominal-chunk index {} is out of range ({} chunks) for '{}:{}'", flatChunkIndex, m_NumChunks, m_FilePath.string(), m_DatasetPath));
      return false;
    }
    const Extent bounds = getChunkBounds(flatChunkIndex, m_TupleShape, m_ChunkShape);
    for(usize dimension = 0; dimension < m_ChunkShape.size(); ++dimension)
    {
      if(bounds.max[dimension] - bounds.min[dimension] + 1 != m_ChunkShape[dimension])
      {
        recordFirstError(errorOut, errorMutex,
                         fmt::format("ParallelChunkCodec: repeated nominal-chunk index {} is clamped in dimension {} for '{}:{}'", flatChunkIndex, dimension, m_FilePath.string(), m_DatasetPath));
        return false;
      }
    }
  }

  const PreparedChunkBytes prepared = prepareChunkBytesImpl(flatChunkIndices.front(), nominalBytes, errorOut, errorMutex);
  if(prepared.filterMask == 0 && prepared.filteredBytes.empty())
  {
    return false;
  }
  const nonstd::span<const std::byte> storedBytes = prepared.filterMask == 0 ? nonstd::span<const std::byte>(prepared.filteredBytes.data(), prepared.filteredBytes.size()) : nominalBytes;
  for(uint64 flatChunkIndex : flatChunkIndices)
  {
    if(!writeCompressedChunkImpl(flatChunkIndex, storedBytes, prepared.filterMask, errorOut, errorMutex))
    {
      return false;
    }
  }
  return true;
}

bool ParallelChunkCodec::deflateSpanIntoChunks(nonstd::span<const std::byte> source, nonstd::span<const uint64> flatChunkIndices, uint64 sourceStartTuple, std::string* firstErrorOut) const
{
  std::mutex errorMutex;
  if(firstErrorOut != nullptr)
  {
    firstErrorOut->clear();
  }
  if(!m_Eligible)
  {
    recordFirstError(firstErrorOut, errorMutex, fmt::format("ParallelChunkCodec: dataset '{}:{}' is not eligible for the parallel deflate write path", m_FilePath.string(), m_DatasetPath));
    return false;
  }
  const usize chunkCount = flatChunkIndices.size();
  if(chunkCount == 0)
  {
    return true;
  }

  // The source describes the tuple range [sourceStartTuple, sourceStartTuple + sourceTupleCount):
  // a whole number of tuples that fits inside the dataset.
  const uint64 totalTuples = static_cast<uint64>(product(m_TupleShape));
  const usize compBytes = m_NumComponents * m_ElementSize;
  if(source.size() % compBytes != 0)
  {
    recordFirstError(firstErrorOut, errorMutex,
                     fmt::format("ParallelChunkCodec: source is {} bytes, not a whole number of {}-byte tuples, for '{}:{}'", source.size(), compBytes, m_FilePath.string(), m_DatasetPath));
    return false;
  }
  const uint64 sourceTupleCount = static_cast<uint64>(source.size() / compBytes);
  if(sourceStartTuple + sourceTupleCount > totalTuples)
  {
    recordFirstError(firstErrorOut, errorMutex,
                     fmt::format("ParallelChunkCodec: source tuple range [{}, {}) exceeds the {} tuples of '{}:{}'", sourceStartTuple, sourceStartTuple + sourceTupleCount, totalTuples,
                                 m_FilePath.string(), m_DatasetPath));
    return false;
  }

  // Validate every index up front (range + tuple-range containment) so a rejection never leaves a
  // half-written batch behind purely due to bad arguments. A chunk's gathers touch flat tuples
  // [ndToFlat(bounds.min), ndToFlat(bounds.max)] (row-major stripes between those endpoints), so
  // endpoint containment covers every gathered byte — and this is the one check the band and the
  // full-array call differ on: a full-array call (sourceStartTuple 0, every chunk) always passes it.
  for(uint64 idx : flatChunkIndices)
  {
    if(idx >= m_NumChunks)
    {
      recordFirstError(firstErrorOut, errorMutex, fmt::format("ParallelChunkCodec: flat chunk index {} is out of range ({} chunks) for '{}:{}'", idx, m_NumChunks, m_FilePath.string(), m_DatasetPath));
      return false;
    }
    const Extent bounds = getChunkBounds(idx, m_TupleShape, m_ChunkShape);
    const uint64 firstTuple = ndToFlat(bounds.min, m_TupleShape);
    const uint64 lastTuple = ndToFlat(bounds.max, m_TupleShape);
    if(firstTuple < sourceStartTuple || lastTuple >= sourceStartTuple + sourceTupleCount)
    {
      recordFirstError(firstErrorOut, errorMutex,
                       fmt::format("ParallelChunkCodec: chunk {} of '{}:{}' covers flat tuples [{}, {}] but the source spans only [{}, {})", idx, m_FilePath.string(), m_DatasetPath, firstTuple,
                                   lastTuple, sourceStartTuple, sourceStartTuple + sourceTupleCount));
      return false;
    }
  }

  struct PendingChunkWrite
  {
    std::vector<std::byte> ownedBytes;
    const std::byte* borrowedData = nullptr;
    usize borrowedSize = 0;
    uint32 filterMask = 0;

    nonstd::span<const std::byte> storedBytes() const
    {
      if(!ownedBytes.empty())
      {
        return {ownedBytes.data(), ownedBytes.size()};
      }
      return {borrowedData, borrowedSize};
    }
  };

  // Keep preparation memory bounded independently of the dataset size. Compression remains
  // parallel inside each batch; the raw HDF5 commits run serially on this calling thread because
  // H5Dwrite_chunk is already serialized by ApiLock and calling it from alternating worker threads
  // intermittently corrupts adjacent chunks on non-thread-safe HDF5 builds.
  const usize nominalChunkBytes = m_NominalChunkElements * m_ElementSize;
  const usize batchChunksByBytes = nominalChunkBytes == 0 ? 1 : std::max<usize>(1, k_MaxPreparedBatchBytes / nominalChunkBytes);
  const usize maxBatchChunks = std::min(k_MaxPreparedBatchChunks, batchChunksByBytes);

  for(usize batchStart = 0; batchStart < chunkCount; batchStart += maxBatchChunks)
  {
    const usize batchChunkCount = std::min(maxBatchChunks, chunkCount - batchStart);
    std::vector<PendingChunkWrite> pendingWrites(batchChunkCount);
    std::atomic<bool> ok{true};

    const auto prepareOne = [&](usize batchIndex) {
      if(!ok.load(std::memory_order_relaxed))
      {
        return;
      }
      const usize sourceIndex = batchStart + batchIndex;
      const uint64 idx = flatChunkIndices[sourceIndex];
      try
      {
        const Extent bounds = getChunkBounds(idx, m_TupleShape, m_ChunkShape);
        usize chunkTuples = 1;
        bool isFullNominalChunk = true;
        for(usize dimension = 0; dimension < m_TupleShape.size(); ++dimension)
        {
          const usize clampedDimension = static_cast<usize>(bounds.max[dimension] - bounds.min[dimension] + 1);
          chunkTuples *= clampedDimension;
          isFullNominalChunk = isFullNominalChunk && clampedDimension == m_ChunkShape[dimension];
        }
        const uint64 firstTuple = ndToFlat(bounds.min, m_TupleShape);
        const uint64 lastTuple = ndToFlat(bounds.max, m_TupleShape);
        const bool isContiguousSourceRun = lastTuple - firstTuple + 1 == chunkTuples;

        std::vector<std::byte> gathered;
        nonstd::span<const std::byte> nominal;
        if(isFullNominalChunk && isContiguousSourceRun)
        {
          const usize sourceByteOffset = static_cast<usize>(firstTuple - sourceStartTuple) * compBytes;
          const usize nominalBytes = chunkTuples * compBytes;
          nominal = source.subspan(sourceByteOffset, nominalBytes);
        }
        else
        {
          gathered = gatherChunkBytes(idx, source, sourceStartTuple);
          nominal = nonstd::span<const std::byte>(gathered.data(), gathered.size());
        }

        PreparedChunkBytes prepared = prepareChunkBytesImpl(idx, nominal, firstErrorOut, errorMutex);
        if(prepared.filterMask == 0 && prepared.filteredBytes.empty())
        {
          ok.store(false, std::memory_order_relaxed);
          return;
        }

        PendingChunkWrite& pending = pendingWrites[batchIndex];
        pending.filterMask = prepared.filterMask;
        if(prepared.filterMask == 0)
        {
          pending.ownedBytes = std::move(prepared.filteredBytes);
        }
        else if(!gathered.empty())
        {
          pending.ownedBytes = std::move(gathered);
        }
        else
        {
          pending.borrowedData = nominal.data();
          pending.borrowedSize = nominal.size();
        }
      } catch(...)
      {
        // No-throw contract: allocation or any other failure becomes a false return.
        recordFirstError(firstErrorOut, errorMutex, fmt::format("ParallelChunkCodec: exception while gathering/compressing chunk {} of '{}:{}'", idx, m_FilePath.string(), m_DatasetPath));
        ok.store(false, std::memory_order_relaxed);
      }
    };

    ParallelForChunkPositions(batchChunkCount, prepareOne);
    if(!ok.load(std::memory_order_relaxed))
    {
      return false;
    }

    for(usize batchIndex = 0; batchIndex < batchChunkCount; ++batchIndex)
    {
      const PendingChunkWrite& pending = pendingWrites[batchIndex];
      if(!writeCompressedChunkImpl(flatChunkIndices[batchStart + batchIndex], pending.storedBytes(), pending.filterMask, firstErrorOut, errorMutex))
      {
        return false;
      }
    }
  }

  return true;
}

} // namespace nx::core::HDF5
