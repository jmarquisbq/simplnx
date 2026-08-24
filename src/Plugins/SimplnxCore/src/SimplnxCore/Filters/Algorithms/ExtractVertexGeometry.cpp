#include "ExtractVertexGeometry.hpp"

#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/IGridGeometry.hpp"
#include "simplnx/DataStructure/Geometry/VertexGeom.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>

using namespace nx::core;

namespace
{
/**
 * @brief Bounded tuple count used for every chunked bulk-I/O pass in this file (mask
 * scanning and the per-array copy/gather). It is fixed and independent of the dataset
 * size, so peak RAM for these passes is O(chunk), never O(totalCells) -- the property
 * that keeps them safe to run against an out-of-core store.
 */
constexpr usize k_ChunkTuples = 65536;

/**
 * @brief Type-dispatched functor that reads exactly one bounded chunk of a
 * single-component mask array (bool or uint8) and translates it into plain bool
 * keep/discard flags.
 *
 * Every consumer of the mask below (the tuple-count pass, the vertex-coordinate pass,
 * and each included array's gather pass) needs the same per-tuple keep/discard
 * decision, but none of them may hold that decision for the whole geometry at once --
 * doing so would mean a std::vector<bool> sized to the total cell count, an
 * O(n_cells) allocation that defeats out-of-core streaming for large volumes. Instead
 * every consumer re-reads the mask through this functor one bounded chunk at a time,
 * discarding each chunk's flags once it has been consumed. The type dispatch happens
 * once per chunk (not once per tuple), so its cost is negligible next to the chunk's
 * bulk I/O.
 */
struct MaskChunkFunctor
{
  template <typename T>
  void operator()(const IDataArray* maskIArray, usize chunkStart, usize count, nonstd::span<bool> outFlags)
  {
    const auto& maskStore = maskIArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    auto buffer = std::make_unique<T[]>(count);
    maskStore.copyIntoBuffer(chunkStart, nonstd::span<T>(buffer.get(), count));
    for(usize i = 0; i < count; i++)
    {
      // Bool values are used as-is; uint8 values are non-zero-is-true, matching
      // MaskCompareUtilities::UInt8MaskCompare::isTrue().
      outFlags[i] = static_cast<bool>(buffer[i]);
    }
  }
};

/**
 * @brief Type-dispatched functor that copies one included cell-data array into the
 * corresponding vertex-attribute-matrix array.
 *
 * The array-by-array copy is the dominant out-of-core cost of this filter. The design
 * this replaced called srcStore[...]/destStore[...] once per tuple: each such
 * operator[] access on an out-of-core DataStore is an independent chunk-cache round
 * trip, so a filter with several included arrays over a large volume paid one round
 * trip per voxel per array. Streaming the source in bounded chunks via
 * copyIntoBuffer(), compacting kept tuples into a bounded output buffer, and flushing
 * via copyFromBuffer() instead touches the OOC store only through bulk chunk I/O,
 * independent of how many tuples are ultimately kept.
 *
 * When a mask is in effect, the keep/discard decision for each chunk is obtained by
 * re-reading that chunk of the (possibly out-of-core) mask through MaskChunkFunctor
 * rather than by indexing into a precomputed per-cell bitmap -- see MaskChunkFunctor's
 * doc comment for why no such bitmap is ever materialized.
 */
struct CopyDataFunctor
{
  template <typename T>
  void operator()(const IDataArray* srcIArray, IDataArray* destIArray, const IDataArray* maskIArray)
  {
    const auto& srcStore = srcIArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    auto& destStore = destIArray->template getIDataStoreRefAs<AbstractDataStore<T>>();

    const usize numComps = srcStore.getNumberOfComponents();
    const usize srcTuples = srcStore.getNumberOfTuples();
    const usize chunkTuples = std::min(srcTuples, k_ChunkTuples);

    if(maskIArray == nullptr)
    {
      // Extract-all: every tuple is kept, so the destination offset always equals the
      // source offset -- a straight chunked copy, no compaction bookkeeping needed.
      auto buffer = std::make_unique<T[]>(chunkTuples * numComps);
      for(usize start = 0; start < srcTuples; start += k_ChunkTuples)
      {
        const usize count = std::min(k_ChunkTuples, srcTuples - start);
        srcStore.copyIntoBuffer(start * numComps, nonstd::span<T>(buffer.get(), count * numComps));
        destStore.copyFromBuffer(start * numComps, nonstd::span<const T>(buffer.get(), count * numComps));
      }
      return;
    }

    // Masked gather: re-stream the mask in lockstep with the source, compact the kept
    // tuples into a bounded rolling output buffer (preserving source order), and flush
    // to the destination whenever the buffer fills or the source is exhausted. Peak
    // RAM is O(chunk), never O(srcTuples) -- the mask flags for a chunk are discarded
    // as soon as that chunk's tuples have been gathered.
    auto inBuffer = std::make_unique<T[]>(chunkTuples * numComps);
    auto outBuffer = std::make_unique<T[]>(chunkTuples * numComps);
    auto flagBuffer = std::make_unique<bool[]>(chunkTuples);
    usize outTuples = 0;
    usize destOffset = 0;

    auto flushOut = [&]() {
      if(outTuples == 0)
      {
        return;
      }
      destStore.copyFromBuffer(destOffset * numComps, nonstd::span<const T>(outBuffer.get(), outTuples * numComps));
      destOffset += outTuples;
      outTuples = 0;
    };

    for(usize chunkStart = 0; chunkStart < srcTuples; chunkStart += k_ChunkTuples)
    {
      const usize chunkCount = std::min(k_ChunkTuples, srcTuples - chunkStart);
      ExecuteDataFunction(MaskChunkFunctor{}, maskIArray->getDataType(), maskIArray, chunkStart, chunkCount, nonstd::span<bool>(flagBuffer.get(), chunkCount));
      srcStore.copyIntoBuffer(chunkStart * numComps, nonstd::span<T>(inBuffer.get(), chunkCount * numComps));

      for(usize i = 0; i < chunkCount; i++)
      {
        if(!flagBuffer[i])
        {
          continue;
        }
        const T* srcTuple = inBuffer.get() + i * numComps;
        T* dstTuple = outBuffer.get() + outTuples * numComps;
        std::copy(srcTuple, srcTuple + numComps, dstTuple);
        outTuples++;
        if(outTuples == k_ChunkTuples)
        {
          flushOut();
        }
      }
    }
    flushOut();
  }
};
} // namespace

// -----------------------------------------------------------------------------
ExtractVertexGeometry::ExtractVertexGeometry(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                             ExtractVertexGeometryInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ExtractVertexGeometry::~ExtractVertexGeometry() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& ExtractVertexGeometry::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> ExtractVertexGeometry::operator()()
{
  const auto& inputGeometry = m_DataStructure.getDataRefAs<IGridGeometry>(m_InputValues->InputGeometryPath);
  auto& vertexGeometry = m_DataStructure.getDataRefAs<VertexGeom>(m_InputValues->VertexGeometryPath);

  SizeVec3 dims = inputGeometry.getDimensions();
  const usize cellCount = std::accumulate(dims.begin(), dims.end(), static_cast<usize>(1), std::multiplies<>());
  usize totalCells = cellCount; // We save this here because it may change based on the use_mask flag.
  usize vertexCount = cellCount;
  DataPath vertexAttributeMatrixDataPath = vertexGeometry.getVertexAttributeMatrixDataPath();

  DataPath maskArrayPath = m_InputValues->MaskArrayPath;
  // The mask array may have gotten moved to the vertex array, if so, we need to find that out.
  if(m_InputValues->UseMask && !m_DataStructure.containsData(m_InputValues->MaskArrayPath))
  {
    if(!m_InputValues->IncludedDataArrayPaths.empty() && m_InputValues->UseMask)
    {
      for(const auto& dataPath : m_InputValues->IncludedDataArrayPaths)
      {
        if(dataPath == m_InputValues->MaskArrayPath)
        {
          maskArrayPath = vertexAttributeMatrixDataPath.createChildPath(m_InputValues->MaskArrayPath.getTargetName());
        }
      }
    }
  }

  m_MessageHandler(IFilter::Message::Type::Info, fmt::format("Preparing arrays for extraction..."));

  // The mask, when in effect, is never materialized as a per-cell bitmap for the
  // whole geometry -- see MaskChunkFunctor's doc comment. Instead this filter makes
  // repeated bounded streaming passes over it: Pass 1 (immediately below) counts how
  // many tuples are kept, sized to the output; Pass 2 (the vertex-coordinate loop, and
  // each included array's gather in CopyDataFunctor) re-reads the mask chunk by chunk
  // to drive compaction. Trading these extra bounded reads of the mask for eliminating
  // an O(n_cells) allocation is the point: the mask array is small relative to the
  // arrays being extracted, so re-streaming it stays cheap next to the alternative of
  // holding a keep/discard decision for the entire volume in RAM at once.
  const IDataArray* maskIDataArray = nullptr;
  if(m_InputValues->UseMask)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    try
    {
      // This call validates that maskArrayPath exists and is a supported mask type
      // (bool/uint8) and preserves the original out_of_range -> MakeErrorResult
      // mapping below. The returned MaskCompare object itself is not used further --
      // the scan below reads the underlying store directly via bulk chunked I/O
      // instead of one isTrue() call per voxel.
      MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, maskArrayPath);
    } catch(const std::out_of_range&)
    {
      // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
      // some other context that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
      return MakeErrorResult(-53900, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", maskArrayPath.toString()));
    }

    maskIDataArray = m_DataStructure.getDataAs<IDataArray>(maskArrayPath);

    // Pass 1 (count): stream the mask in bounded chunks and count kept tuples. This
    // never allocates more than one chunk's worth of flags, regardless of totalCells.
    const usize chunkTuples = std::min(totalCells, k_ChunkTuples);
    auto flagBuffer = std::make_unique<bool[]>(chunkTuples);
    vertexCount = 0;
    for(usize chunkStart = 0; chunkStart < totalCells; chunkStart += k_ChunkTuples)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize count = std::min(k_ChunkTuples, totalCells - chunkStart);
      ExecuteDataFunction(MaskChunkFunctor{}, maskIDataArray->getDataType(), maskIDataArray, chunkStart, count, nonstd::span<bool>(flagBuffer.get(), count));
      for(usize i = 0; i < count; i++)
      {
        if(flagBuffer[i])
        {
          vertexCount++;
        }
      }
    }
    vertexGeometry.resizeVertexList(vertexCount);
  }

  if(m_ShouldCancel)
  {
    return {};
  }

  // Use the APIs from the IGeometryGrid to get the XYZ coord for the center
  // of each cell and then set that into the new VertexGeometry. getCoordsf() is pure
  // geometry math (origin/spacing, or small per-axis bounds arrays for RectGrid), and
  // the vertex SharedVertexList is always an in-core store, so this pass never touches
  // an out-of-core cell array for the coordinates themselves. When a mask is active,
  // it still re-reads the (possibly out-of-core) mask in bounded chunks -- Pass 2 of
  // the streaming design described above -- to decide which cells to keep.
  m_MessageHandler(IFilter::Message::Type::Info, fmt::format("Generating vertex geometry"));

  IGeometry::SharedVertexList& vertices = vertexGeometry.getVerticesRef();
  auto& verticesDataStore = vertices.getDataStoreRef();
  if(m_InputValues->UseMask)
  {
    const usize chunkTuples = std::min(totalCells, k_ChunkTuples);
    auto flagBuffer = std::make_unique<bool[]>(chunkTuples);
    usize vertIdx = 0;
    for(usize chunkStart = 0; chunkStart < totalCells; chunkStart += k_ChunkTuples)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize count = std::min(k_ChunkTuples, totalCells - chunkStart);
      ExecuteDataFunction(MaskChunkFunctor{}, maskIDataArray->getDataType(), maskIDataArray, chunkStart, count, nonstd::span<bool>(flagBuffer.get(), count));
      for(usize i = 0; i < count; i++)
      {
        if(flagBuffer[i])
        {
          const Point3D<float32> coords = inputGeometry.getCoordsf(chunkStart + i);
          verticesDataStore.setTuple(vertIdx, coords.toArray());
          vertIdx++;
        }
      }
    }
  }
  else
  {
    for(usize idx = 0; idx < totalCells; idx++)
    {
      const Point3D<float32> coords = inputGeometry.getCoordsf(idx);
      verticesDataStore.setTuple(idx, coords.toArray());
    }
  }

  m_MessageHandler(IFilter::Message::Type::Info, fmt::format("Copying cell data to vertex geometry"));

  // Since we made copies of the DataArrays, we can safely resize the entire Attribute Matrix,
  // which will resize all the contained DataArrays
  AttributeMatrix& vertexAttrMatrix = vertexGeometry.getVertexAttributeMatrixRef();
  vertexAttrMatrix.resizeTuples({vertexCount});

  for(const auto& dataArrayPath : m_InputValues->IncludedDataArrayPaths)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    const auto* srcIDataArray = m_DataStructure.getDataAs<IDataArray>(dataArrayPath);
    DataPath destDataArrayPath = vertexAttributeMatrixDataPath.createChildPath(srcIDataArray->getName());
    auto* destDataArray = m_DataStructure.getDataAs<IDataArray>(destDataArrayPath);
    ExecuteDataFunction(CopyDataFunctor{}, srcIDataArray->getDataType(), srcIDataArray, destDataArray, maskIDataArray);
  }

  return {};
}
