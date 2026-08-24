#include "EBSDSegmentFeatures.hpp"

#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/Geometry/IGridGeometry.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"

#include <algorithm>
#include <memory>

using namespace nx::core;

// -----------------------------------------------------------------------------
EBSDSegmentFeatures::EBSDSegmentFeatures(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, EBSDSegmentFeaturesInputValues* inputValues)
: SegmentFeatures(dataStructure, shouldCancel, mesgHandler)
, m_InputValues(inputValues)
{
  m_OrientationOps = ebsdlib::LaueOps::GetAllOrientationOps();
  m_IsPeriodic = inputValues->IsPeriodic;
}

// -----------------------------------------------------------------------------
EBSDSegmentFeatures::~EBSDSegmentFeatures() noexcept = default;

// -----------------------------------------------------------------------------
// Segments an EBSD dataset into crystallographic features (grains) by grouping
// contiguous voxels whose crystal orientations are within a user-specified
// misorientation tolerance. Two voxels are grouped into the same feature only
// if they share the same phase and their misorientation (computed via the
// appropriate LaueOps symmetry operator) is below the threshold.
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
//   4. Optionally randomize FeatureIds so that spatially adjacent grains get
//      non-sequential IDs, improving visual contrast in color-mapped renders.
// -----------------------------------------------------------------------------
Result<> EBSDSegmentFeatures::operator()()
{
  this->m_NeighborScheme = m_InputValues->NeighborScheme;
  auto* gridGeom = m_DataStructure.getDataAs<IGridGeometry>(m_InputValues->ImageGeometryPath);

  m_QuatsArray = m_DataStructure.getDataAs<Float32Array>(m_InputValues->QuatsArrayPath);
  m_CellPhases = m_DataStructure.getDataAs<Int32Array>(m_InputValues->CellPhasesArrayPath);
  if(m_InputValues->UseMask)
  {
    try
    {
      m_GoodVoxelsArray = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
    } catch(const std::out_of_range& exception)
    {
      // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
      // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
      std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->MaskArrayPath.toString());
      return MakeErrorResult(-485090, message);
    }
  }
  m_CrystalStructures = m_DataStructure.getDataAs<UInt32Array>(m_InputValues->CrystalStructuresArrayPath);

  m_FeatureIdsArray = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);
  m_FeatureIdsArray->fill(0); // initialize the output array with zeros

  SizeVec3 udims = gridGeom->getDimensions();
  auto allocateResult = allocateSliceBuffers(static_cast<int64>(udims[0]), static_cast<int64>(udims[1]));
  if(allocateResult.invalid())
  {
    return allocateResult;
  }

  auto& featureIdsStore = m_FeatureIdsArray->getDataStoreRef();
  const auto* maskArray = m_InputValues->UseMask ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath) : nullptr;
  const bool usesOutOfCoreInput = IsOutOfCore(*m_QuatsArray) || IsOutOfCore(*m_CellPhases) || IsOutOfCore(*m_CrystalStructures) || (maskArray != nullptr && IsOutOfCore(*maskArray));
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
    return MakeErrorResult(-87000, "No Features were detected: no Cell was eligible to seed a Feature. Every Cell is either excluded by the Mask or has a Phase value of 0 (unindexed).");
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
// Checks whether a single voxel is eligible for segmentation. A voxel is valid
// if it passes the mask and has a crystallographic phase > 0.
//
// Slice buffer fast path:
//   When m_UseSliceBuffers is true, the method first checks whether the voxel's
//   Z-slice is currently loaded in either LRU slot. If it is resident, mask
//   and phase values are read directly from the in-memory
//   m_MaskBuffer and m_PhaseBuffer arrays, avoiding an on-disk I/O round-trip.
//
// Inactive-buffer fallback:
//   Direct access is retained for callers outside executeCCL. During CCL,
//   missing slice state is rejected instead of issuing a DataStore read.
// -----------------------------------------------------------------------------
bool EBSDSegmentFeatures::isValidVoxel(int64 point) const
{
  if(m_UseSliceBuffers)
  {
    const int64 iz = point / m_BufSliceSize;
    const int slot = m_BufferedSliceZ[0] == iz ? 0 : (m_BufferedSliceZ[1] == iz ? 1 : -1);
    if(slot >= 0)
    {
      const usize sliceSize = static_cast<usize>(m_BufSliceSize);
      const usize off = static_cast<usize>(slot) * sliceSize + static_cast<usize>(point - iz * m_BufSliceSize);
      if(m_InputValues->UseMask && m_MaskBuffer[off] == 0)
      {
        return false;
      }
      if(m_PhaseBuffer[off] <= 0)
      {
        return false;
      }
      return true;
    }
    return false;
  }

  // In-core fallback used only when slice buffering is inactive.
  if(m_InputValues->UseMask && !m_GoodVoxelsArray->isTrue(point))
  {
    return false;
  }
  AbstractDataStore<int32>& cellPhases = m_CellPhases->getDataStoreRef();
  if(cellPhases[point] <= 0)
  {
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Determines whether two neighboring voxels are crystallographically similar
// enough to belong to the same feature.
//
// Slice buffer fast path:
//   When both voxels' Z-slices are present in the rolling 2-slot buffer, all
//   data is read from the in-memory buffers (m_QuatBuffer, m_PhaseBuffer,
//   m_MaskBuffer). The buffer offset for each point is computed as:
//     slot * sliceSize + (point - iz * sliceSize)
//   For quaternions, an additional x4 factor accounts for the 4 components
//   per voxel. The method then:
//     1. Checks point2's mask validity.
//     2. Checks that point2's phase > 0 and both phases match.
//     3. Looks up the Laue class and verifies it is in range.
//     4. Constructs QuatD objects from the buffered quaternion components.
//     5. Computes misorientation via LaueOps::calculateMisorientation().
//     6. Returns true if the misorientation angle < MisorientationTolerance.
//
// Inactive-buffer fallback:
//   Direct comparison is retained for callers outside executeCCL. Periodic CCL
//   explicitly loads both required slices and never takes this path.
// -----------------------------------------------------------------------------
bool EBSDSegmentFeatures::areNeighborsSimilar(int64 point1, int64 point2) const
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
      const int32 phase1 = m_PhaseBuffer[off1];
      const int32 phase2 = m_PhaseBuffer[off2];
      if(phase2 <= 0)
      {
        return false;
      }
      if(phase1 != phase2)
      {
        return false;
      }

      int32 laueClass = static_cast<int32>(m_CrystalStructuresCache[static_cast<usize>(phase1)]);
      if(static_cast<usize>(laueClass) >= m_OrientationOps.size())
      {
        return false;
      }

      const usize q1Base = static_cast<usize>(slot1) * sliceSize * 4 + static_cast<usize>(point1 - iz1 * m_BufSliceSize) * 4;
      const usize q2Base = static_cast<usize>(slot2) * sliceSize * 4 + static_cast<usize>(point2 - iz2 * m_BufSliceSize) * 4;

      // Identical quaternions have exactly zero misorientation. This is the
      // overwhelmingly common case inside grains and avoids the substantially
      // more expensive symmetry-operator calculation without changing the
      // tolerance semantics.
      if(m_QuatBuffer[q1Base] == m_QuatBuffer[q2Base] && m_QuatBuffer[q1Base + 1] == m_QuatBuffer[q2Base + 1] && m_QuatBuffer[q1Base + 2] == m_QuatBuffer[q2Base + 2] &&
         m_QuatBuffer[q1Base + 3] == m_QuatBuffer[q2Base + 3])
      {
        return 0.0F < m_InputValues->MisorientationTolerance;
      }

      const ebsdlib::QuatD q1(m_QuatBuffer[q1Base], m_QuatBuffer[q1Base + 1], m_QuatBuffer[q1Base + 2], m_QuatBuffer[q1Base + 3]);
      const ebsdlib::QuatD q2(m_QuatBuffer[q2Base], m_QuatBuffer[q2Base + 1], m_QuatBuffer[q2Base + 2], m_QuatBuffer[q2Base + 3]);

      ebsdlib::AxisAngleDType axisAngle = m_OrientationOps[laueClass]->calculateMisorientation(q1, q2);
      float32 w = static_cast<float32>(axisAngle[3]);

      return w < m_InputValues->MisorientationTolerance;
    }
    return false;
  }

  // In-core fallback used only when slice buffering is inactive.
  if(!isValidVoxel(point2))
  {
    return false;
  }

  AbstractDataStore<int32>& cellPhases = m_CellPhases->getDataStoreRef();

  if(cellPhases[point1] != cellPhases[point2])
  {
    return false;
  }

  int32 laueClass = (*m_CrystalStructures)[cellPhases[point1]];
  if(static_cast<usize>(laueClass) >= m_OrientationOps.size())
  {
    return false;
  }

  Float32Array& quats = *m_QuatsArray;
  if(quats[point1 * 4] == quats[point2 * 4] && quats[point1 * 4 + 1] == quats[point2 * 4 + 1] && quats[point1 * 4 + 2] == quats[point2 * 4 + 2] && quats[point1 * 4 + 3] == quats[point2 * 4 + 3])
  {
    return 0.0F < m_InputValues->MisorientationTolerance;
  }

  const ebsdlib::QuatD q1(quats[point1 * 4], quats[point1 * 4 + 1], quats[point1 * 4 + 2], quats[point1 * 4 + 3]);
  const ebsdlib::QuatD q2(quats[point2 * 4], quats[point2 * 4 + 1], quats[point2 * 4 + 2], quats[point2 * 4 + 3]);

  ebsdlib::AxisAngleDType axisAngle = m_OrientationOps[laueClass]->calculateMisorientation(q1, q2);
  float32 w = static_cast<float32>(axisAngle[3]);

  return w < m_InputValues->MisorientationTolerance;
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
//   - m_QuatBuffer  : 2 * sliceSize * 4 floats  (quaternion: 4 components/voxel)
//   - m_PhaseBuffer : 2 * sliceSize int32 values (one phase ID per voxel)
//   - m_MaskBuffer  : 2 * sliceSize uint8 values (one mask flag per voxel)
//
// Both m_BufferedSliceZ slots are initialized to -1 (no slice loaded).
// m_UseSliceBuffers is set to true so that isValidVoxel() and
// areNeighborsSimilar() will use the fast buffer path.
// -----------------------------------------------------------------------------
Result<> EBSDSegmentFeatures::allocateSliceBuffers(int64 dimX, int64 dimY)
{
  m_BufSliceSize = dimX * dimY;
  const usize sliceSize = static_cast<usize>(m_BufSliceSize);
  m_QuatBuffer.resize(2 * sliceSize * 4);
  m_PhaseBuffer.resize(2 * sliceSize);
  m_MaskBuffer.resize(2 * sliceSize);
  m_BufferedSliceZ[0] = -1;
  m_BufferedSliceZ[1] = -1;
  m_BufferUseSequence = {0, 0};
  m_NextBufferUseSequence = 1;
  m_UseSliceBuffers = true;

  // Cache crystal structures locally to avoid per-voxel OOC access.
  // This array is tiny (one entry per phase) but gets accessed ~24M times
  // during the CCL inner loop; going through an OOC DataStore each time is
  // the dominant bottleneck.
  const usize numPhases = m_CrystalStructures->getNumberOfTuples();
  m_CrystalStructuresCache.resize(numPhases);
  auto readResult = m_CrystalStructures->getDataStoreRef().copyIntoBuffer(0, nonstd::span<uint32>(m_CrystalStructuresCache.data(), numPhases));
  if(readResult.invalid())
  {
    return readResult;
  }
  return {};
}

// -----------------------------------------------------------------------------
// Releases the slice buffers after executeCCL() completes, freeing the memory
// back to the system. Called in operator() after the CCL algorithm finishes.
// Resets m_UseSliceBuffers to false and both
// m_BufferedSliceZ slots to -1. The vectors are replaced with default-
// constructed (empty) instances to guarantee memory deallocation.
// -----------------------------------------------------------------------------
void EBSDSegmentFeatures::deallocateSliceBuffers()
{
  m_UseSliceBuffers = false;
  m_QuatBuffer = std::vector<float32>();
  m_PhaseBuffer = std::vector<int32>();
  m_MaskBuffer = std::vector<uint8>();
  m_CrystalStructuresCache = std::vector<uint32>();
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
//   - Quaternions (4 float32 per voxel) into m_QuatBuffer
//   - Phase IDs (1 int32 per voxel) into m_PhaseBuffer
//   - Mask flags (1 uint8 per voxel) into m_MaskBuffer; if masking is disabled,
//     all mask values are set to 1 (valid)
// -----------------------------------------------------------------------------
Result<> EBSDSegmentFeatures::prepareForSlice(int64 iz, int64 dimX, int64 dimY, int64 dimZ)
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
  const usize quatSlotOffset = slotOffset * 4;
  const int64 baseIndex = iz * m_BufSliceSize;

  // Bulk-read quaternions (4 components per voxel) for this slice
  AbstractDataStore<float32>& quatStore = m_QuatsArray->getDataStoreRef();
  auto quatReadResult = quatStore.copyIntoBuffer(static_cast<usize>(baseIndex) * 4, nonstd::span<float32>(m_QuatBuffer.data() + quatSlotOffset, sliceSize * 4));
  if(quatReadResult.invalid())
  {
    return quatReadResult;
  }

  // Bulk-read phase IDs for this slice
  AbstractDataStore<int32>& phaseStore = m_CellPhases->getDataStoreRef();
  auto phaseReadResult = phaseStore.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<int32>(m_PhaseBuffer.data() + slotOffset, sliceSize));
  if(phaseReadResult.invalid())
  {
    return phaseReadResult;
  }

  // Bulk-read mask flags for this slice
  if(m_InputValues->UseMask && m_GoodVoxelsArray != nullptr)
  {
    auto& maskArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->MaskArrayPath);
    if(maskArray.getDataType() == DataType::uint8)
    {
      auto& typedStore = maskArray.getIDataStoreRefAs<AbstractDataStore<uint8>>();
      auto maskReadResult = typedStore.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<uint8>(m_MaskBuffer.data() + slotOffset, sliceSize));
      if(maskReadResult.invalid())
      {
        return maskReadResult;
      }
    }
    else if(maskArray.getDataType() == DataType::boolean)
    {
      auto& typedStore = maskArray.getIDataStoreRefAs<AbstractDataStore<bool>>();
      auto boolBuf = std::make_unique<bool[]>(sliceSize);
      auto maskReadResult = typedStore.copyIntoBuffer(static_cast<usize>(baseIndex), nonstd::span<bool>(boolBuf.get(), sliceSize));
      if(maskReadResult.invalid())
      {
        return maskReadResult;
      }
      for(usize i = 0; i < sliceSize; i++)
      {
        m_MaskBuffer[slotOffset + i] = boolBuf[i] ? 1 : 0;
      }
    }
    else
    {
      return MakeErrorResult(-485091, "EBSDSegmentFeatures mask storage must be Bool or UInt8.");
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
