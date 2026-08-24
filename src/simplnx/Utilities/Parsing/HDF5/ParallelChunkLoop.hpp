#pragma once

#include "simplnx/Common/Types.hpp"
#include "simplnx/Utilities/Parsing/HDF5/ParallelChunkCodec.hpp"

#include <nonstd/span.hpp>

#ifdef SIMPLNX_ENABLE_MULTICORE
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/partitioner.h>
#endif

#include <algorithm>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>

namespace nx::core::HDF5
{
template <class Body>
void ParallelForChunkPositions(usize chunkCount, const Body& body)
{
#ifdef SIMPLNX_ENABLE_MULTICORE
  if(chunkCount > 1)
  {
    tbb::static_partitioner partitioner;
    tbb::parallel_for(
        tbb::blocked_range<usize>(0, chunkCount, 1),
        [&body](const tbb::blocked_range<usize>& range) {
          for(usize localIndex = range.begin(); localIndex < range.end(); ++localIndex)
          {
            body(localIndex);
          }
        },
        partitioner);
    return;
  }
#endif
  for(usize localIndex = 0; localIndex < chunkCount; ++localIndex)
  {
    body(localIndex);
  }
}

/**
 * @brief Runs a per-chunk @p loader then @p sink for every index in @p chunkIndices, in parallel.
 *
 * @par What it does
 * For each local position @c i in @p chunkIndices this calls @p loader with the chunk index at
 * that position, then hands the produced @c Result to @p sink as @c sink(i, std::move(result)).
 * Work is submitted to oneTBB's process-wide scheduler, whose persistent worker pool is reused
 * across calls. Each task owns disjoint local-index values, so a @p sink that writes @c result[i]
 * into a pre-sized container needs no lock of its own.
 *
 * @par Why a task partition and a callable seam
 * The task split gives each invocation a disjoint local index with no shared write target, which
 * is what lets the sink be lock-free. Threading @c loader (rather than a single
 * serial loop) is the point: for a deflate-compressed chunk the decompression, not the disk, is
 * the cost, and it can run off any process-wide HDF5 lock on separate worker threads. The loader
 * and sink are injected as callables because callers vary only in what a loaded chunk produces
 * (an owned byte buffer, a move-only handle, ...) and what they do with it (scatter into an
 * output span, retain the handle), while the thread harness, partition, and error policy are identical.
 *
 * @par Per-chunk exception policy
 * - An @c UnallocatedChunkError thrown by @p loader marks a never-written (fill-value) chunk of a
 *   sparse dataset. Such a chunk has nothing to load and is not a chunk-level error, so it is
 *   SKIPPED: @p sink is not called for it and its slot is left as the caller pre-sized it.
 * - ANY other exception (from @p loader or @p sink) is a genuine failure. The first such exception
 *   is recorded under a mutex; remaining chunks still run, and after the scheduled batch finishes
 *   the first recorded exception is rethrown. This preserves the "do as much work as possible,
 *   then surface the first real error" contract.
 *
 * @tparam Result The value @p loader produces for one chunk and @p sink consumes. May be move-only.
 * @param chunkIndices The chunk indices to load, one per local position; @c count == 0 is a no-op.
 * @param loader Produces the @c Result for a given chunk index; may throw @c UnallocatedChunkError
 *               to signal a skippable unallocated chunk.
 * @param sink Consumes @c (localIndex, Result&&); called at most once per non-skipped chunk, and
 *             only on the worker that owns that local index.
 *
 * @throw std::exception (or subclass) The first non-@c UnallocatedChunkError exception thrown by
 *        @p loader or @p sink, rethrown after the scheduled batch finishes.
 */
template <class Result>
void ParallelLoadChunks(nonstd::span<const uint64> chunkIndices, const std::function<Result(uint64 chunkIndex)>& loader, const std::function<void(usize localIndex, Result&&)>& sink)
{
  const usize chunkCount = chunkIndices.size();
  if(chunkCount == 0)
  {
    return;
  }

  std::mutex errMutex;
  std::exception_ptr firstError = nullptr;

  const auto loadOne = [&](usize i) {
    try
    {
      Result result = loader(chunkIndices[i]);
      // Tasks own disjoint local indices, so a sink that writes result[i] needs no lock.
      sink(i, std::move(result));
    } catch(const UnallocatedChunkError&)
    {
      // A never-written (fill-value) chunk of a sparse dataset has nothing to load; skip it
      // rather than fail the whole load. The sink is not called, leaving this local index's
      // slot exactly as the caller pre-sized it.
      return;
    } catch(...)
    {
      // Any OTHER exception is a genuine failure and must propagate: record the first and keep
      // going so other chunks still load; it is rethrown after the scheduler finishes the batch.
      std::lock_guard<std::mutex> lk(errMutex);
      if(!firstError)
      {
        firstError = std::current_exception();
      }
    }
  };

  ParallelForChunkPositions(chunkCount, loadOne);

  if(firstError)
  {
    std::rethrow_exception(firstError);
  }
}
} // namespace nx::core::HDF5
