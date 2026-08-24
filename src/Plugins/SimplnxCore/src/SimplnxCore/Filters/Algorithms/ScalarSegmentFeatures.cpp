#include "ScalarSegmentFeatures.hpp"

#include <algorithm>
#include <memory>

#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/Geometry/IGridGeometry.hpp"
#include "simplnx/Filter/Actions/CreateArrayAction.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

using namespace nx::core;

#define CX_DEFAULT_CONSTRUCTORS(className)                                                                                                                                                             \
  className(const className&) = delete;                                                                                                                                                                \
  className(className&&) noexcept = delete;                                                                                                                                                            \
  className& operator=(const className&) = delete;                                                                                                                                                     \
  className& operator=(className&&) noexcept = delete;

namespace
{
constexpr int64 k_IncorrectInputArray = -600;
constexpr int64 k_MissingInputArray = -601;
constexpr int64 k_MissingOrIncorrectGoodVoxelsArray = -602;

/**
 * @brief The TSpecificCompareFunctorBool class extends @see CompareFunctor to compare boolean data
 */
class TSpecificCompareFunctorBool : public SegmentFeatures::CompareFunctor
{
public:
  using DataArrayType = BoolArray;
  CX_DEFAULT_CONSTRUCTORS(TSpecificCompareFunctorBool)

  /** @brief Borrows a Boolean array and its validated tuple count for exact comparisons. */
  TSpecificCompareFunctorBool(IDataArray* data, int64 length)
  : m_Length(length)
  , m_Data(dynamic_cast<DataArrayType*>(data))
  {
  }
  TSpecificCompareFunctorBool() = default;
  ~TSpecificCompareFunctorBool() override = default;

  /** @brief Returns true when both indices are in range and contain the same Boolean value. */
  bool compare(int64 index, int64 neighIndex) override
  {
    if(index >= m_Length || neighIndex >= m_Length)
    {
      return false;
    }
    return (*m_Data)[neighIndex] == (*m_Data)[index];
  }

private:
  int64 m_Length = 0;              // Length of the Data Array
  DataArrayType* m_Data = nullptr; // The data that is being compared
};

/**
 * @brief Resident fallback comparator for one numeric scalar type.
 * The scanline CCL normally compares preloaded float64 slice values; this
 * adapter preserves exact typed behavior for comparisons outside that window.
 */
template <class T>
class TSpecificCompareFunctor : public SegmentFeatures::CompareFunctor
{
public:
  CX_DEFAULT_CONSTRUCTORS(TSpecificCompareFunctor)

  using DataArrayType = DataArray<T>;
  using DataStoreType = AbstractDataStore<T>;

  /** @brief Borrows the typed store and stores the tolerance in its native type. */
  TSpecificCompareFunctor(IDataArray* data, int64 length, T tolerance)
  : m_Length(length)
  , m_Tolerance(tolerance)
  , m_Data(data->template getIDataStoreRefAs<DataStoreType>())
  {
  }
  TSpecificCompareFunctor() = default;
  ~TSpecificCompareFunctor() override = default;

  /** @brief Performs an unsigned-safe absolute-difference comparison. */
  bool compare(int64 index, int64 neighIndex) override
  {
    if(index >= m_Length || neighIndex >= m_Length)
    {
      return false;
    }

    if(m_Data[index] >= m_Data[neighIndex])
    {
      return (m_Data[index] - m_Data[neighIndex]) <= m_Tolerance;
    }
    return (m_Data[neighIndex] - m_Data[index]) <= m_Tolerance;
  }

private:
  int64 m_Length = 0;                // Length of the Data Array
  T m_Tolerance = static_cast<T>(0); // The tolerance of the comparison
  DataStoreType& m_Data;             // The data that is being compared
};

/**
 * @brief Functor for type-dispatched filling of a scalar slice buffer.
 * Bulk-reads an entire slice via copyIntoBuffer() into a local typed buffer,
 * then converts to float64 for uniform comparison. Uses std::make_unique<T[]>
 * instead of std::vector<T> to avoid std::vector<bool> specialization issues.
 */
struct FillScalarSliceBufferFunctor
{
  /**
   * @brief Bulk-reads one typed slice and widens it into the shared float64 buffer.
   * @return The backing-store read failure, or success after all values convert.
   */
  template <typename T>
  Result<> operator()(IDataArray* dataArray, int64 baseIndex, usize sliceSize, std::vector<float64>& buffer, usize bufferOffset)
  {
    auto& store = dataArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    // Bulk-read the entire slice into a local typed buffer, then convert to float64
    auto tempBuffer = std::make_unique<T[]>(sliceSize);
    auto readResult = store.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<T>(tempBuffer.get(), sliceSize));
    if(readResult.invalid())
    {
      return readResult;
    }
    for(usize i = 0; i < sliceSize; i++)
    {
      buffer[bufferOffset + i] = static_cast<float64>(tempBuffer[i]);
    }
    return {};
  }
};
} // namespace

ScalarSegmentFeatures::ScalarSegmentFeatures(DataStructure& dataStructure, ScalarSegmentFeaturesInputValues* inputValues, const std::atomic_bool& shouldCancel,
                                             const IFilter::MessageHandler& mesgHandler)
: SegmentFeatures(dataStructure, shouldCancel, mesgHandler)
, m_InputValues(inputValues)
{
  m_IsPeriodic = inputValues->IsPeriodic;
}

ScalarSegmentFeatures::~ScalarSegmentFeatures() noexcept = default;

// -----------------------------------------------------------------------------
// Segments an image/rectilinear grid into features (regions) by grouping
// contiguous voxels whose scalar values differ by no more than a user-specified
// tolerance. This is a general-purpose segmentation: it works on any single-
// component scalar array (int8 through float64, plus boolean), unlike the
// orientation-based EBSD and CAxis segment filters.
//
// Comparator setup:
//   A type-dispatched CompareFunctor is instantiated via a switch on the input
//   array's DataType. Each TSpecificCompareFunctor<T> stores the tolerance cast
//   to the native type and performs |a - b| <= tolerance using unsigned-safe
//   subtraction. Boolean arrays use a dedicated TSpecificCompareFunctorBool
//   that checks for exact equality (no tolerance concept). If the input array
//   has more than one component, a default CompareFunctor that always returns
//   false is used, effectively preventing any grouping.
//
// Segmentation:
//   The base-class connected-component labeling algorithm (executeCCL()) walks
//   the volume one Z-slice at a time. Slice buffers are allocated first so the
//   comparison overrides can read pre-loaded input data, then released once the
//   algorithm completes.
//
// Post-processing:
//   1. Validate that at least one feature was found (error if not).
//   2. Resize the Feature AttributeMatrix to (m_FoundFeatures + 1) tuples so
//      that all per-feature arrays (Active, etc.) have the correct size.
//      Index 0 is reserved as an invalid/background feature.
//   3. Initialize the Active array: fill with 1 (active), then set index 0
//      to 0 to mark it as the reserved background slot.
//   4. Optionally randomize FeatureIds so that spatially adjacent features get
//      non-sequential IDs, improving visual contrast in color-mapped renders.
// -----------------------------------------------------------------------------
Result<> ScalarSegmentFeatures::operator()()
{
  this->m_NeighborScheme = m_InputValues->NeighborScheme;
  if(m_InputValues->UseMask)
  {
    try
    {
      m_GoodVoxels = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
    } catch(const std::out_of_range& exception)
    {
      // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
      // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
      std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->MaskArrayPath.toString());
      return MakeErrorResult(-54110, message);
    }
  }

  auto* gridGeom = m_DataStructure.getDataAs<IGridGeometry>(m_InputValues->ImageGeometryPath);

  m_FeatureIdsArray = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);

  auto* inputDataArray = m_DataStructure.getDataAs<IDataArray>(m_InputValues->InputDataPath);
  m_InputDataArray = inputDataArray;
  usize inDataPoints = inputDataArray->getNumberOfTuples();
  nx::core::DataType dataType = inputDataArray->getDataType();

  switch(dataType)
  {
  case nx::core::DataType::int8: {
    m_CompareFunctor = std::make_shared<::TSpecificCompareFunctor<int8>>(inputDataArray, inDataPoints, static_cast<int8>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::uint8: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<uint8>>(inputDataArray, inDataPoints, static_cast<uint8>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::boolean: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctorBool>(inputDataArray, inDataPoints);
    break;
  }
  case nx::core::DataType::int16: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<int16>>(inputDataArray, inDataPoints, static_cast<int16>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::uint16: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<uint16>>(inputDataArray, inDataPoints, static_cast<uint16>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::int32: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<int32>>(inputDataArray, inDataPoints, static_cast<int32>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::uint32: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<uint32>>(inputDataArray, inDataPoints, static_cast<uint32>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::int64: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<int64>>(inputDataArray, inDataPoints, static_cast<int64>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::uint64: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<uint64>>(inputDataArray, inDataPoints, static_cast<uint64>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::float32: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<float32>>(inputDataArray, inDataPoints, static_cast<float32>(m_InputValues->ScalarTolerance));
    break;
  }
  case nx::core::DataType::float64: {
    m_CompareFunctor = std::make_shared<TSpecificCompareFunctor<float64>>(inputDataArray, inDataPoints, static_cast<float64>(m_InputValues->ScalarTolerance));
    break;
  }
  default:
    break;
  }
  if(inputDataArray->getNumberOfComponents() != 1)
  {
    m_CompareFunctor = std::make_shared<SegmentFeatures::CompareFunctor>(); // The default CompareFunctor which ALWAYS returns false for the comparison
  }

  SizeVec3 udims = gridGeom->getDimensions();
  allocateSliceBuffers(static_cast<int64>(udims[0]), static_cast<int64>(udims[1]));

  auto& featureIdsStore = m_FeatureIdsArray->getDataStoreRef();
  const auto* maskArray = m_InputValues->UseMask ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath) : nullptr;
  const bool usesOutOfCoreInput = IsOutOfCore(*m_InputDataArray) || (maskArray != nullptr && IsOutOfCore(*maskArray));
  Result<> segmentResult = executeCCL(gridGeom, featureIdsStore, usesOutOfCoreInput);

  deallocateSliceBuffers();

  if(segmentResult.invalid())
  {
    return segmentResult;
  }
  if(m_ShouldCancel)
  {
    return {};
  }

  // Sanity check the result.
  if(m_FoundFeatures < 1)
  {
    return MakeErrorResult(-87000, "No Features were detected: no Cell was eligible to seed a Feature. Every Cell is excluded by the Mask.");
  }

  // Resize the Feature Attribute Matrix
  ShapeType tDims = {static_cast<usize>(m_FoundFeatures + 1)};
  auto& cellFeaturesAM = m_DataStructure.getDataRefAs<AttributeMatrix>(m_InputValues->CellFeatureAttributeMatrixPath);
  cellFeaturesAM.resizeTuples(tDims); // This will resize the active array

  // make sure all values are initialized and "re-reserve" index 0
  auto* activeArray = m_DataStructure.getDataAs<UInt8Array>(m_InputValues->ActiveArrayPath);
  activeArray->getDataStore()->fill(1);
  (*activeArray)[0] = 0;

  // Randomize the feature Ids for purely visual clarify. Having random Feature Ids
  // allows users visualizing the data to better discern each grain otherwise the coloring
  // would look like a smooth gradient. This is a user input parameter
  if(m_InputValues->RandomizeFeatureIds)
  {
    randomizeFeatureIds(m_FeatureIdsArray, m_FoundFeatures + 1);
  }

  return {};
}

// -----------------------------------------------------------------------------
// Checks whether a single voxel is eligible for segmentation. For scalar
// segmentation, validity only depends on the mask -- there is no phase check
// because scalar data is phase-agnostic.
//
// Slice buffer fast path:
//   When m_UseSliceBuffers is true, the method first checks whether the voxel's
//   Z-slice is currently loaded in either LRU slot. If it is resident, the
//   mask value is read directly from the in-memory m_MaskBuffer,
//   avoiding an on-disk I/O round-trip.
//
// Inactive-buffer fallback:
//   Direct access is retained for callers outside executeCCL. During CCL,
//   missing slice state is rejected instead of issuing a DataStore read.
// -----------------------------------------------------------------------------
bool ScalarSegmentFeatures::isValidVoxel(int64 point) const
{
  if(m_UseSliceBuffers)
  {
    const int64 iz = point / m_BufSliceSize;
    const int slot = m_BufferedSliceZ[0] == iz ? 0 : (m_BufferedSliceZ[1] == iz ? 1 : -1);
    if(slot >= 0)
    {
      const usize off = static_cast<usize>(slot) * static_cast<usize>(m_BufSliceSize) + static_cast<usize>(point - iz * m_BufSliceSize);
      if(m_InputValues->UseMask && m_MaskBuffer[off] == 0)
      {
        return false;
      }
      return true;
    }
    return false;
  }

  // In-core fallback used only when slice buffering is inactive.
  if(m_InputValues->UseMask && !m_GoodVoxels->isTrue(point))
  {
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Determines whether two neighboring voxels have sufficiently similar scalar
// values to belong to the same feature.
//
// Slice buffer fast path:
//   When both voxels' Z-slices are present in the rolling 2-slot buffer, all
//   data is read from the in-memory buffers (m_ScalarBuffer, m_MaskBuffer).
//   The buffer offset for each point is computed as:
//     slot * sliceSize + (point - iz * sliceSize)
//   The method then:
//     1. Checks point2's mask validity.
//     2. Reads both scalar values from m_ScalarBuffer as float64.
//     3. Computes |val1 - val2| and returns true if <= ScalarTolerance.
//   All scalar types are stored as float64 in the buffer so that a single
//   comparison path works regardless of the original data type. The tolerance
//   is also cast to float64 for the comparison.
//
// Inactive-buffer fallback:
//   Direct comparison is retained for callers outside executeCCL. Periodic CCL
//   explicitly loads both required slices and never takes this path.
// -----------------------------------------------------------------------------
bool ScalarSegmentFeatures::areNeighborsSimilar(int64 point1, int64 point2) const
{
  if(m_UseSliceBuffers)
  {
    const int64 iz1 = point1 / m_BufSliceSize;
    const int slot1 = m_BufferedSliceZ[0] == iz1 ? 0 : (m_BufferedSliceZ[1] == iz1 ? 1 : -1);
    const int64 iz2 = point2 / m_BufSliceSize;
    const int slot2 = m_BufferedSliceZ[0] == iz2 ? 0 : (m_BufferedSliceZ[1] == iz2 ? 1 : -1);

    if(slot1 >= 0 && slot2 >= 0)
    {
      const usize sliceSize = static_cast<usize>(m_BufSliceSize);
      const usize off1 = static_cast<usize>(slot1) * sliceSize + static_cast<usize>(point1 - iz1 * m_BufSliceSize);
      const usize off2 = static_cast<usize>(slot2) * sliceSize + static_cast<usize>(point2 - iz2 * m_BufSliceSize);

      // Check point2 validity
      if(m_InputValues->UseMask && m_MaskBuffer[off2] == 0)
      {
        return false;
      }

      // Compare scalar values from the pre-loaded buffer
      float64 val1 = m_ScalarBuffer[off1];
      float64 val2 = m_ScalarBuffer[off2];
      float64 diff = val1 >= val2 ? (val1 - val2) : (val2 - val1);
      return diff <= static_cast<float64>(m_InputValues->ScalarTolerance);
    }
    return false;
  }

  // In-core fallback used only when slice buffering is inactive.
  if(!isValidVoxel(point2))
  {
    return false;
  }
  return m_CompareFunctor->compare(point1, point2);
}

// -----------------------------------------------------------------------------
// Allocates the rolling 2-slot slice buffers used by the CCL algorithm.
// Called once in operator(), before executeCCL().
//
// Each slot holds one full XY slice (dimX * dimY voxels). Two slots are needed
// because the CCL algorithm compares the current slice (iz) with the previous
// slice (iz-1), so both must be in memory simultaneously.
//
// Buffers allocated:
//   - m_ScalarBuffer : 2 * sliceSize float64 values (one scalar per voxel,
//                      stored as float64 regardless of the original data type
//                      so that a single comparison path works for all types)
//   - m_MaskBuffer   : 2 * sliceSize uint8 values (one mask flag per voxel)
//
// Both m_BufferedSliceZ slots are initialized to -1 (no slice loaded).
// m_UseSliceBuffers is set to true so that isValidVoxel() and
// areNeighborsSimilar() will use the fast buffer path.
// -----------------------------------------------------------------------------
void ScalarSegmentFeatures::allocateSliceBuffers(int64 dimX, int64 dimY)
{
  m_BufSliceSize = dimX * dimY;
  const usize sliceSize = static_cast<usize>(m_BufSliceSize);
  m_ScalarBuffer.resize(2 * sliceSize);
  m_MaskBuffer.resize(2 * sliceSize);
  m_BufferedSliceZ[0] = -1;
  m_BufferedSliceZ[1] = -1;
  m_BufferUseSequence = {0, 0};
  m_NextBufferUseSequence = 1;
  m_UseSliceBuffers = true;
}

// -----------------------------------------------------------------------------
// Releases the slice buffers after executeCCL() completes, freeing the memory
// back to the system. Called in operator() after the CCL algorithm finishes.
// Resets m_UseSliceBuffers to false and both
// m_BufferedSliceZ slots to -1. The vectors are replaced with default-
// constructed (empty) instances to guarantee memory deallocation.
// -----------------------------------------------------------------------------
void ScalarSegmentFeatures::deallocateSliceBuffers()
{
  m_UseSliceBuffers = false;
  m_ScalarBuffer = std::vector<float64>();
  m_MaskBuffer = std::vector<uint8>();
  m_BufferedSliceZ[0] = -1;
  m_BufferedSliceZ[1] = -1;
  m_BufferUseSequence = {0, 0};
  m_NextBufferUseSequence = 1;
}

// -----------------------------------------------------------------------------
// Pre-loads voxel data for a single Z-slice into the rolling 2-slot buffer,
// called by executeCCL() before processing each slice.
//
// Rolling buffer design:
//   Two LRU slots retain the current/previous forward-pass slices and any pair
//   explicitly requested by periodic-boundary merging. Resident slices are not
//   re-read.
//
// Data loaded per slice:
//   - Scalar values (1 float64 per voxel) into m_ScalarBuffer. The type
//     dispatch uses ExecuteDataFunctionNoBool with FillScalarSliceBufferFunctor
//     to convert the original typed data (int8..float64) to float64. Boolean
//     arrays are handled separately because ExecuteDataFunctionNoBool excludes
//     bool; they are converted to 0.0/1.0 manually.
//   - Mask flags (1 uint8 per voxel) into m_MaskBuffer; if masking is disabled,
//     all mask values are set to 1 (valid).
// -----------------------------------------------------------------------------
Result<> ScalarSegmentFeatures::prepareForSlice(int64 iz, int64 dimX, int64 dimY, int64 dimZ)
{
  if(iz < 0)
  {
    m_UseSliceBuffers = false;
    return {};
  }
  if(!m_UseSliceBuffers)
  {
    return {};
  }

  int slot = m_BufferedSliceZ[0] == iz ? 0 : (m_BufferedSliceZ[1] == iz ? 1 : -1);
  if(slot >= 0)
  {
    m_BufferUseSequence[static_cast<usize>(slot)] = m_NextBufferUseSequence++;
    return {};
  }
  if(m_BufferedSliceZ[0] < 0)
  {
    slot = 0;
  }
  else if(m_BufferedSliceZ[1] < 0)
  {
    slot = 1;
  }
  else
  {
    slot = m_BufferUseSequence[0] <= m_BufferUseSequence[1] ? 0 : 1;
  }

  const usize sliceSize = static_cast<usize>(m_BufSliceSize);
  const usize slotOffset = static_cast<usize>(slot) * sliceSize;
  const int64 baseIndex = iz * m_BufSliceSize;

  // Fill scalar data buffer using type dispatch
  DataType dataType = m_InputDataArray->getDataType();
  if(dataType == DataType::boolean)
  {
    auto& store = m_InputDataArray->template getIDataStoreRefAs<AbstractDataStore<bool>>();
    // Bulk-read the entire boolean slice, then convert to float64
    auto boolBuf = std::make_unique<bool[]>(sliceSize);
    auto readResult = store.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<bool>(boolBuf.get(), sliceSize));
    if(readResult.invalid())
    {
      return readResult;
    }
    for(usize i = 0; i < sliceSize; i++)
    {
      m_ScalarBuffer[slotOffset + i] = boolBuf[i] ? 1.0 : 0.0;
    }
  }
  else
  {
    auto readResult = ExecuteDataFunctionNoBool(FillScalarSliceBufferFunctor{}, dataType, m_InputDataArray, baseIndex, sliceSize, m_ScalarBuffer, slotOffset);
    if(readResult.invalid())
    {
      return readResult;
    }
  }

  // Fill mask buffer using bulk reads to avoid per-element OOC overhead
  if(m_InputValues->UseMask && m_GoodVoxels != nullptr)
  {
    auto& maskArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->MaskArrayPath);
    if(maskArray.getDataType() == DataType::uint8)
    {
      // Bulk-read uint8 mask data directly into the mask buffer
      auto& typedStore = maskArray.getIDataStoreRefAs<AbstractDataStore<uint8>>();
      auto readResult = typedStore.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<uint8>(m_MaskBuffer.data() + slotOffset, sliceSize));
      if(readResult.invalid())
      {
        return readResult;
      }
    }
    else if(maskArray.getDataType() == DataType::boolean)
    {
      // Bulk-read boolean mask data into a temp buffer, then convert to uint8
      auto& typedStore = maskArray.getIDataStoreRefAs<AbstractDataStore<bool>>();
      auto boolBuf = std::make_unique<bool[]>(sliceSize);
      auto readResult = typedStore.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<bool>(boolBuf.get(), sliceSize));
      if(readResult.invalid())
      {
        return readResult;
      }
      for(usize i = 0; i < sliceSize; i++)
      {
        m_MaskBuffer[slotOffset + i] = boolBuf[i] ? 1 : 0;
      }
    }
    else
    {
      return MakeErrorResult(-54111, "ScalarSegmentFeatures mask storage must be Bool or UInt8.");
    }
  }
  else
  {
    std::fill(m_MaskBuffer.begin() + slotOffset, m_MaskBuffer.begin() + slotOffset + sliceSize, static_cast<uint8>(1));
  }

  m_BufferedSliceZ[slot] = iz;
  m_BufferUseSequence[static_cast<usize>(slot)] = m_NextBufferUseSequence++;
  return {};
}
