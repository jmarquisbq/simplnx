#include "CAxisSegmentFeatures.hpp"

#include "OrientationAnalysis/utilities/OrientationUtilities.hpp"

#include "simplnx/Common/Constants.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/IGridGeometry.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"

#include <EbsdLib/Orientation/OrientationFwd.hpp>
#include <EbsdLib/Orientation/OrientationMatrix.hpp>
#include <EbsdLib/Orientation/Quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace nx::core;
using namespace nx::core::OrientationUtilities;

// -----------------------------------------------------------------------------
CAxisSegmentFeatures::CAxisSegmentFeatures(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, CAxisSegmentFeaturesInputValues* inputValues)
: SegmentFeatures(dataStructure, shouldCancel, mesgHandler)
, m_InputValues(inputValues)
{
}

// -----------------------------------------------------------------------------
CAxisSegmentFeatures::~CAxisSegmentFeatures() noexcept = default;

// -----------------------------------------------------------------------------
// Segments a hexagonal EBSD dataset into features (grains) based on c-axis
// alignment. Two neighboring voxels are grouped into the same feature when
// their crystallographic c-axes (the [0001] direction) are aligned within a
// user-specified angular tolerance. Unlike EBSDSegmentFeatures which uses full
// misorientation via LaueOps, this filter only considers the c-axis direction,
// which is useful for analyzing basal texture in hexagonal materials.
//
// Pre-validation:
//   Before segmentation, every cell's phase is checked against the crystal
//   structure table. All phases must be hexagonal (Hexagonal_High 6/mmm or
//   Hexagonal_Low 6/m); if any non-hexagonal phase is found, the filter
//   returns an error because c-axis alignment is only meaningful for HCP.
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
Result<> CAxisSegmentFeatures::operator()()
{
  m_NeighborScheme = m_InputValues->NeighborScheme;
  // The geometry parameter accepts Image and RectGrid geometries; both derive from IGridGeometry.
  auto* gridGeom = m_DataStructure.getDataAs<IGridGeometry>(m_InputValues->ImageGeometryPath);
  m_QuatsArray = m_DataStructure.getDataAs<Float32Array>(m_InputValues->QuatsArrayPath);
  m_CellPhases = m_DataStructure.getDataAs<Int32Array>(m_InputValues->CellPhasesArrayPath);
  if(m_InputValues->UseMask)
  {
    try
    {
      m_GoodVoxelsArray = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
    } catch(const std::out_of_range&)
    {
      // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
      // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
      return MakeErrorResult(-8362, fmt::format("Mask Array DataPath '{}' does not exist or is not of the correct type (Bool | UInt8).", m_InputValues->MaskArrayPath.toString()));
    }
  }

  // Validate each positive, unmasked phase once. Cache both the ensemble table and phase batches
  // so validation stays efficient for out-of-core stores.
  const auto& crystalStructures = m_DataStructure.getDataRefAs<UInt32Array>(m_InputValues->CrystalStructuresArrayPath);
  const usize numEnsembles = crystalStructures.getNumberOfTuples();
  std::vector<uint32> crystalStructuresCache(numEnsembles);
  auto crystalStructuresReadResult = crystalStructures.getDataStoreRef().copyIntoBuffer(0, nonstd::span<uint32>(crystalStructuresCache.data(), numEnsembles));
  if(crystalStructuresReadResult.invalid())
  {
    return crystalStructuresReadResult;
  }
  std::vector<uint8> phaseValidated(numEnsembles, 0);

  const usize numCells = m_CellPhases->getNumberOfTuples();
  const SizeVec3 scanDims = gridGeom->getDimensions();
  const usize scanBatchSize = std::max<usize>(1, static_cast<usize>(scanDims[0]) * static_cast<usize>(scanDims[1]));
  std::vector<int32> phasesBuffer(scanBatchSize);
  std::vector<uint8> maskBuffer(scanBatchSize, 1);
  IDataArray* maskArray = m_InputValues->UseMask ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath) : nullptr;
  auto& phasesStore = m_CellPhases->getDataStoreRef();
  for(usize offset = 0; offset < numCells; offset += scanBatchSize)
  {
    const usize batchSize = std::min(scanBatchSize, numCells - offset);
    auto phaseReadResult = phasesStore.copyIntoBuffer(offset, nonstd::span<int32>(phasesBuffer.data(), batchSize));
    if(phaseReadResult.invalid())
    {
      return phaseReadResult;
    }
    if(maskArray != nullptr && maskArray->getDataType() == DataType::uint8)
    {
      auto& maskStore = maskArray->getIDataStoreRefAs<AbstractDataStore<uint8>>();
      auto maskReadResult = maskStore.copyIntoBuffer(offset, nonstd::span<uint8>(maskBuffer.data(), batchSize));
      if(maskReadResult.invalid())
      {
        return maskReadResult;
      }
    }
    else if(maskArray != nullptr && maskArray->getDataType() == DataType::boolean)
    {
      auto& maskStore = maskArray->getIDataStoreRefAs<AbstractDataStore<bool>>();
      auto boolBuffer = std::make_unique<bool[]>(batchSize);
      auto maskReadResult = maskStore.copyIntoBuffer(offset, nonstd::span<bool>(boolBuffer.get(), batchSize));
      if(maskReadResult.invalid())
      {
        return maskReadResult;
      }
      for(usize i = 0; i < batchSize; ++i)
      {
        maskBuffer[i] = boolBuffer[i] ? 1 : 0;
      }
    }
    for(usize i = 0; i < batchSize; i++)
    {
      const int32 currentPhaseIdx = phasesBuffer[i];
      if(currentPhaseIdx <= 0)
      {
        continue;
      }
      if(static_cast<usize>(currentPhaseIdx) < numEnsembles && phaseValidated[currentPhaseIdx] != 0)
      {
        continue;
      }

      const usize cellIdx = offset + i;
      if(maskArray != nullptr && maskBuffer[i] == 0)
      {
        continue;
      }
      if(static_cast<usize>(currentPhaseIdx) >= numEnsembles)
      {
        return MakeErrorResult(-8364, fmt::format("Cell {} has a phase value of {} but the Crystal Structures array '{}' only has {} entries.", cellIdx, currentPhaseIdx,
                                                  m_InputValues->CrystalStructuresArrayPath.toString(), numEnsembles));
      }

      const auto crystalStructureType = crystalStructuresCache[static_cast<usize>(currentPhaseIdx)];
      if(crystalStructureType != ebsdlib::CrystalStructure::Hexagonal_High && crystalStructureType != ebsdlib::CrystalStructure::Hexagonal_Low)
      {
        return MakeErrorResult(-8363, fmt::format("Input data is using {} type crystal structures but segmenting features via c-axis misorientation requires every phase that participates in the "
                                                  "segmentation to be either Hexagonal-Low 6/m or Hexagonal-High 6/mmm type crystal structures.",
                                                  CrystalStructureEnumToString(crystalStructureType)));
      }
      phaseValidated[static_cast<usize>(currentPhaseIdx)] = 1;
    }
  }

  m_FeatureIdsArray = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);
  m_FeatureIdsArray->fill(0);

  const SizeVec3 udims = gridGeom->getDimensions();
  allocateSliceBuffers(static_cast<int64>(udims[0]), static_cast<int64>(udims[1]));

  auto& featureIdsStore = m_FeatureIdsArray->getDataStoreRef();
  const bool usesOutOfCoreInput = IsOutOfCore(*m_QuatsArray) || IsOutOfCore(*m_CellPhases) || IsOutOfCore(crystalStructures) || (maskArray != nullptr && IsOutOfCore(*maskArray));
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
  auto& cellFeatureAM = m_DataStructure.getDataRefAs<AttributeMatrix>(m_InputValues->CellFeatureAttributeMatrixPath);
  cellFeatureAM.resizeTuples(tDims); // This will resize the active array

  // make sure all values are initialized and "re-reserve" index 0
  auto* activeArray = m_DataStructure.getDataAs<UInt8Array>(m_InputValues->ActiveArrayPath);
  activeArray->getDataStore()->fill(1);
  (*activeArray)[0] = 0;

  // Randomize the feature Ids for purely visual clarify. Having random Feature Ids
  // allows users visualizing the data to better discern each grain otherwise the coloring
  // would look like a smooth gradient. This is a user input parameter
  if(m_InputValues->RandomizeFeatureIds)
  {
    ClusterUtilities::RandomizeFeatureIds(m_FeatureIdsArray->getDataStoreRef(), m_FoundFeatures + 1);
  }

  return {};
}

// -----------------------------------------------------------------------------
// Checks whether a single voxel is eligible for segmentation. A voxel is valid
// if it passes the mask and has a crystallographic phase > 0.
//
// Slice buffer fast path:
//   When m_UseSliceBuffers is true, the method checks whether the voxel's
//   Z-slice is currently loaded in one of the two buffer slots. The
//   slot lookup checks both m_BufferedSliceZ[0] and m_BufferedSliceZ[1] to
//   find which slot (if any) holds the target slice. If found, mask and phase
//   values are read from the in-memory m_MaskBuffer and m_PhaseBuffer arrays,
//   avoiding an on-disk I/O round-trip.
//
// Inactive-buffer fallback:
//   Direct access is retained for callers outside executeCCL. During CCL,
//   missing slice state is rejected instead of issuing a DataStore read.
// -----------------------------------------------------------------------------
bool CAxisSegmentFeatures::isValidVoxel(int64 point) const
{
  if(m_UseSliceBuffers)
  {
    int64 sliceZ = point / m_BufSliceSize;
    if(sliceZ == m_BufferedSliceZ[0] || sliceZ == m_BufferedSliceZ[1])
    {
      int64 slot = (sliceZ == m_BufferedSliceZ[0]) ? 0 : 1;
      int64 offset = point - sliceZ * m_BufSliceSize;
      int64 bufIdx = slot * m_BufSliceSize + offset;
      // Check mask
      if(m_InputValues->UseMask && m_MaskBuffer[bufIdx] == 0)
      {
        return false;
      }
      // Check phase
      if(m_PhaseBuffer[bufIdx] <= 0)
      {
        return false;
      }
      return true;
    }
    return false;
  }

  // Fallback: direct array access
  if(m_InputValues->UseMask && !m_GoodVoxelsArray->isTrue(point))
  {
    return false;
  }
  Int32Array& cellPhases = *m_CellPhases;
  if(cellPhases[point] <= 0)
  {
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Determines whether two neighboring voxels have sufficiently aligned c-axes
// to belong to the same feature.
//
// Slice buffer fast path:
//   When both voxels' Z-slices are present in the rolling 2-slot buffer, all
//   data is read from the in-memory buffers (m_QuatBuffer, m_PhaseBuffer,
//   m_MaskBuffer). The buffer index for each point is computed as:
//     slot * sliceSize + (point - sliceZ * sliceSize)
//   For quaternions, an additional x4 factor accounts for the 4 components
//   per voxel. The method then:
//     1. Checks point2's mask validity.
//     2. Checks that point2's phase > 0 and both phases match.
//     3. Constructs QuatF objects from the buffered quaternion components.
//     4. Converts each quaternion to an orientation matrix, transposes it, and
//        multiplies by [0,0,1] to get the sample-frame c-axis direction.
//     5. Normalizes both c-axis vectors and computes the dot product.
//     6. Clamps the dot product to [-1,1] and takes acos() to get the
//        misalignment angle w.
//     7. Returns true if w <= tolerance OR (pi - w) <= tolerance (because
//        parallel and antiparallel c-axes are crystallographically equivalent).
//
// Inactive-buffer fallback:
//   Direct comparison is retained for callers outside executeCCL. Periodic CCL
//   explicitly loads both required slices and never takes this path.
// -----------------------------------------------------------------------------
bool CAxisSegmentFeatures::areNeighborsSimilar(int64 point1, int64 point2) const
{
  if(m_UseSliceBuffers)
  {
    int64 sliceZ1 = point1 / m_BufSliceSize;
    int64 sliceZ2 = point2 / m_BufSliceSize;
    bool buf1 = (sliceZ1 == m_BufferedSliceZ[0] || sliceZ1 == m_BufferedSliceZ[1]);
    bool buf2 = (sliceZ2 == m_BufferedSliceZ[0] || sliceZ2 == m_BufferedSliceZ[1]);

    if(buf1 && buf2)
    {
      int64 slot1 = (sliceZ1 == m_BufferedSliceZ[0]) ? 0 : 1;
      int64 slot2 = (sliceZ2 == m_BufferedSliceZ[0]) ? 0 : 1;
      int64 off1 = point1 - sliceZ1 * m_BufSliceSize;
      int64 off2 = point2 - sliceZ2 * m_BufSliceSize;
      int64 bufIdx1 = slot1 * m_BufSliceSize + off1;
      int64 bufIdx2 = slot2 * m_BufSliceSize + off2;

      // Check point2 validity (mask + phase)
      if(m_InputValues->UseMask && m_MaskBuffer[bufIdx2] == 0)
      {
        return false;
      }
      if(m_PhaseBuffer[bufIdx2] <= 0)
      {
        return false;
      }

      // Must be same phase
      if(m_PhaseBuffer[bufIdx1] != m_PhaseBuffer[bufIdx2])
      {
        return false;
      }

      // Read quaternions from buffer
      int64 qIdx1 = bufIdx1 * 4;
      int64 qIdx2 = bufIdx2 * 4;
      const ebsdlib::QuatF q1(m_QuatBuffer[qIdx1], m_QuatBuffer[qIdx1 + 1], m_QuatBuffer[qIdx1 + 2], m_QuatBuffer[qIdx1 + 3]);
      const ebsdlib::QuatF q2(m_QuatBuffer[qIdx2], m_QuatBuffer[qIdx2 + 1], m_QuatBuffer[qIdx2 + 2], m_QuatBuffer[qIdx2 + 3]);

      const ebsdlib::OrientationMatrixFType oMatrix1 = q1.toOrientationMatrix();
      const ebsdlib::OrientationMatrixFType oMatrix2 = q2.toOrientationMatrix();

      const Eigen::Vector3f cAxis{0.0f, 0.0f, 1.0f};
      Eigen::Vector3f c1 = oMatrix1.transpose() * cAxis;
      Eigen::Vector3f c2 = oMatrix2.transpose() * cAxis;

      c1.normalize();
      c2.normalize();

      float32 w = std::clamp(((c1[0] * c2[0]) + (c1[1] * c2[1]) + (c1[2] * c2[2])), -1.0F, 1.0F);
      w = std::acos(w);

      return w <= m_InputValues->MisorientationTolerance || (Constants::k_PiD - w) <= m_InputValues->MisorientationTolerance;
    }
    return false;
  }

  // Fallback: direct array access
  if(!isValidVoxel(point2))
  {
    return false;
  }

  Int32Array& cellPhases = *m_CellPhases;

  // Must be same phase
  if(cellPhases[point1] != cellPhases[point2])
  {
    return false;
  }

  // Calculate c-axis misalignment
  const Eigen::Vector3f cAxis{0.0f, 0.0f, 1.0f};
  Float32Array& quats = *m_QuatsArray;

  const ebsdlib::QuatF q1(quats[point1 * 4], quats[point1 * 4 + 1], quats[point1 * 4 + 2], quats[point1 * 4 + 3]);
  const ebsdlib::QuatF q2(quats[point2 * 4], quats[point2 * 4 + 1], quats[point2 * 4 + 2], quats[point2 * 4 + 3]);

  const ebsdlib::OrientationMatrixFType oMatrix1 = q1.toOrientationMatrix();
  const ebsdlib::OrientationMatrixFType oMatrix2 = q2.toOrientationMatrix();

  Eigen::Vector3f c1 = oMatrix1.transpose() * cAxis;
  Eigen::Vector3f c2 = oMatrix2.transpose() * cAxis;

  c1.normalize();
  c2.normalize();

  float32 w = std::clamp(((c1[0] * c2[0]) + (c1[1] * c2[1]) + (c1[2] * c2[2])), -1.0F, 1.0F);
  w = std::acos(w);

  return w <= m_InputValues->MisorientationTolerance || (Constants::k_PiD - w) <= m_InputValues->MisorientationTolerance;
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
void CAxisSegmentFeatures::allocateSliceBuffers(int64 dimX, int64 dimY)
{
  m_BufSliceSize = dimX * dimY;
  int64 totalSlots = 2 * m_BufSliceSize;
  m_QuatBuffer.resize(static_cast<usize>(totalSlots * 4));
  m_PhaseBuffer.resize(static_cast<usize>(totalSlots));
  m_MaskBuffer.resize(static_cast<usize>(totalSlots));
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
// m_BufferedSliceZ slots to -1. Uses clear() + shrink_to_fit() on each vector
// to guarantee memory deallocation.
// -----------------------------------------------------------------------------
void CAxisSegmentFeatures::deallocateSliceBuffers()
{
  m_UseSliceBuffers = false;
  m_QuatBuffer.clear();
  m_QuatBuffer.shrink_to_fit();
  m_PhaseBuffer.clear();
  m_PhaseBuffer.shrink_to_fit();
  m_MaskBuffer.clear();
  m_MaskBuffer.shrink_to_fit();
  m_BufferedSliceZ[0] = -1;
  m_BufferedSliceZ[1] = -1;
  m_BufferUseSequence = {0, 0};
  m_NextBufferUseSequence = 1;
  m_BufSliceSize = 0;
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
//
// -----------------------------------------------------------------------------
Result<> CAxisSegmentFeatures::prepareForSlice(int64 iz, int64 dimX, int64 dimY, int64 dimZ)
{
  if(iz < 0)
  {
    m_UseSliceBuffers = false;
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

  const int64 sliceStart = iz * m_BufSliceSize;
  const int64 bufOffset = static_cast<int64>(slot) * m_BufSliceSize;
  const usize sliceSize = static_cast<usize>(m_BufSliceSize);
  const usize slotOffset = static_cast<usize>(bufOffset);

  // Bulk-read quaternions (4 components per voxel) for this slice
  AbstractDataStore<float32>& quatStore = m_QuatsArray->getDataStoreRef();
  auto quatReadResult = quatStore.copyIntoBuffer(static_cast<usize>(sliceStart) * 4, nonstd::span<float32>(m_QuatBuffer.data() + slotOffset * 4, sliceSize * 4));
  if(quatReadResult.invalid())
  {
    return quatReadResult;
  }

  // Bulk-read phase IDs for this slice
  AbstractDataStore<int32>& phaseStore = m_CellPhases->getDataStoreRef();
  auto phaseReadResult = phaseStore.copyIntoBuffer(static_cast<usize>(sliceStart), nonstd::span<int32>(m_PhaseBuffer.data() + slotOffset, sliceSize));
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
      auto maskReadResult = typedStore.copyIntoBuffer(static_cast<usize>(sliceStart), nonstd::span<uint8>(m_MaskBuffer.data() + slotOffset, sliceSize));
      if(maskReadResult.invalid())
      {
        return maskReadResult;
      }
    }
    else if(maskArray.getDataType() == DataType::boolean)
    {
      auto& typedStore = maskArray.getIDataStoreRefAs<AbstractDataStore<bool>>();
      auto boolBuf = std::make_unique<bool[]>(sliceSize);
      auto maskReadResult = typedStore.copyIntoBuffer(static_cast<usize>(sliceStart), nonstd::span<bool>(boolBuf.get(), sliceSize));
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
      return MakeErrorResult(-8365, "CAxisSegmentFeatures mask storage must be Bool or UInt8.");
    }
  }
  else
  {
    // If no mask, mark everything as valid
    std::fill(m_MaskBuffer.begin() + slotOffset, m_MaskBuffer.begin() + slotOffset + sliceSize, static_cast<uint8>(1));
  }
  m_BufferedSliceZ[static_cast<usize>(slot)] = iz;
  m_BufferUseSequence[static_cast<usize>(slot)] = m_NextBufferUseSequence++;
  return {};
}
