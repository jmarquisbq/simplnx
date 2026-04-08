#include "BadDataNeighborOrientationCheckWorklist.hpp"

#include "BadDataNeighborOrientationCheck.hpp"

#include "simplnx/Common/Numbers.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/NeighborUtilities.hpp"

#include <EbsdLib/LaueOps/LaueOps.h>

#include <deque>

using namespace nx::core;

// -----------------------------------------------------------------------------
BadDataNeighborOrientationCheckWorklist::BadDataNeighborOrientationCheckWorklist(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                                                 const BadDataNeighborOrientationCheckInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
BadDataNeighborOrientationCheckWorklist::~BadDataNeighborOrientationCheckWorklist() noexcept = default;

// -----------------------------------------------------------------------------
/**
 * @brief Flips bad voxels to good using a worklist-based propagation algorithm.
 * In-core path: Phase 1 counts matching good neighbors per bad voxel.
 * Phase 2 iteratively flips eligible voxels using a deque worklist,
 * propagating new eligibility to neighbors immediately.
 */
Result<> BadDataNeighborOrientationCheckWorklist::operator()()
{
  // Compute the tolerance in double precision: numbers::pi_v<float> is the closest float to true pi, which is
  // slightly *larger* than true pi; converting via float makes the radian tolerance ~5e-9 rad larger than the
  // mathematically true k*pi/180. For boundary-exact misorientations (e.g., test fixtures landing on exactly the
  // user-supplied tolerance), the float-converted tolerance can incorrectly include cases that should fail strict <.
  // Using double-pi makes the conversion faithful and the strict < tolerance comparison match the analytical oracle.
  const double misorientationTolerance = static_cast<double>(m_InputValues->MisorientationTolerance) * numbers::pi_v<double> / 180.0;

  const auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeomPath);
  SizeVec3 udims = imageGeom.getDimensions();
  const auto& cellPhases = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->CellPhasesArrayPath);
  const auto& quats = m_DataStructure.getDataRefAs<Float32Array>(m_InputValues->QuatsArrayPath);
  const auto& crystalStructures = m_DataStructure.getDataRefAs<UInt32Array>(m_InputValues->CrystalStructuresArrayPath);
  const usize totalPoints = quats.getNumberOfTuples();

  std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare;
  try
  {
    maskCompare = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
  } catch(const std::out_of_range& exception)
  {
    // Defensive: the path was verified during preflight, but this algorithm may be called outside the standard
    // IFilter Preflight/Execute path.
    return MakeErrorResult(-54900,
                           fmt::format("Mask Array at '{}' could not be loaded; expected Bool or UInt8 backing. Underlying error: {}", m_InputValues->MaskArrayPath.toString(), exception.what()));
  }

  std::array<int64, 3> dims = {
      static_cast<int64>(udims[0]),
      static_cast<int64>(udims[1]),
      static_cast<int64>(udims[2]),
  };

  const int64 xyStride = dims[0] * dims[1];

  // VoxelNeighbors<Image3D>::k_FaceNeighborCount = 6 is the maximum possible face-neighbor count.
  // computeValidFaceNeighbors() runtime-skips +/-Z neighbors when dims[2] == 1 (2D images), so this
  // 3D-typed array correctly handles 2D images without any change here.
  constexpr FaceNeighborType k_NumFaceNeighbors = VoxelNeighbors<Image3D>::k_FaceNeighborCount;
  const std::array<int64, k_NumFaceNeighbors> neighborVoxelIndexOffsets = initializeFaceNeighborOffsets(dims);
  constexpr std::array<FaceNeighborType, k_NumFaceNeighbors> faceNeighborInternalIdx = initializeFaceNeighborInternalIdx();

  const std::vector<ebsdlib::LaueOps::Pointer> orientationOps = ebsdlib::LaueOps::GetAllOrientationOps();

  // Validate that every entry in the CrystalStructures ensemble array is a valid Laue-group index
  // (< orientationOps.size()). Catches malformed inputs such as a legacy CreateEnsembleInfo sentinel
  // value (999) at ensemble index 0 before they cause an out-of-bounds dereference in the per-voxel
  // loop below. The UnknownCrystalStructure value is explicitly allowed as a sentinel; voxels whose
  // phase resolves to it will be skipped by the cellPhases > 0 guard. CrystalStructures is typically
  // tiny (2-4 entries), so the cost is negligible.
  const usize numOrientationOps = orientationOps.size();
  for(usize i = 0; i < crystalStructures.getSize(); ++i)
  {
    if(crystalStructures[i] >= numOrientationOps && crystalStructures[i] != ebsdlib::CrystalStructure::UnknownCrystalStructure)
    {
      return MakeErrorResult(
          -54901, fmt::format("Crystal structure at ensemble index {} has value {}, which is not a valid Laue-group index. Valid range is [0, {}).", i, crystalStructures[i], numOrientationOps));
    }
  }

  // Per-voxel running count of within-tolerance face-neighbors. Allocated proportional to the
  // input geometry size: 4 bytes per voxel (~4 GB for a 1B-voxel dataset). Cannot be in-place
  // on the mask array because the algorithm needs to distinguish "newly flipped" from "still bad".
  std::vector<int32> neighborCount(totalPoints, 0);

  MessageHelper messageHelper(m_MessageHandler);
  ThrottledMessenger throttledMessenger = messageHelper.createThrottledMessenger();

  // ===== Phase 1: Count matching good neighbors for each bad voxel =====
  for(usize voxelIndex = 0; voxelIndex < totalPoints; voxelIndex++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    throttledMessenger.sendThrottledMessage([&] { return fmt::format("Processing Data {:.2f}% completed", CalculatePercentComplete(voxelIndex, totalPoints)); });
    // "Bad" voxels are those whose mask value is false; only these get processed.
    if(!maskCompare->isTrue(voxelIndex))
    {
      ebsdlib::QuatD quat1(quats[voxelIndex * 4], quats[voxelIndex * 4 + 1], quats[voxelIndex * 4 + 2], quats[voxelIndex * 4 + 3]);
      quat1.positiveOrientation();
      const uint32 laueClassIndex = crystalStructures[cellPhases[voxelIndex]];
      // Defensive: skip voxels whose phase resolves to an out-of-range Laue index (e.g., the
      // UnknownCrystalStructure sentinel allowed by the validation above). Without this, the
      // orientationOps[laueClassIndex] dereference below would be out-of-bounds.
      if(laueClassIndex >= numOrientationOps)
      {
        continue;
      }

      const int64 xIdx = static_cast<int64>(voxelIndex) % dims[0];
      const int64 yIdx = (static_cast<int64>(voxelIndex) / dims[0]) % dims[1];
      const int64 zIdx = static_cast<int64>(voxelIndex) / xyStride;

      const std::array<bool, k_NumFaceNeighbors> isValidFaceNeighbor = computeValidFaceNeighbors(xIdx, yIdx, zIdx, dims);
      for(const auto& faceIndex : faceNeighborInternalIdx)
      {
        if(!isValidFaceNeighbor[faceIndex])
        {
          continue;
        }
        const int64 neighborPoint = static_cast<int64>(voxelIndex) + neighborVoxelIndexOffsets[faceIndex];

        if(maskCompare->isTrue(neighborPoint))
        {
          if(cellPhases[voxelIndex] == cellPhases[neighborPoint] && cellPhases[voxelIndex] > 0)
          {
            ebsdlib::QuatD quat2(quats[neighborPoint * 4], quats[neighborPoint * 4 + 1], quats[neighborPoint * 4 + 2], quats[neighborPoint * 4 + 3]);
            quat2.positiveOrientation();
            // Compute the Axis_Angle misorientation between those 2 quaternions
            ebsdlib::AxisAngleDType axisAngle = orientationOps[laueClassIndex]->calculateMisorientation(quat1, quat2);
            if(axisAngle[3] < misorientationTolerance)
            {
              neighborCount[voxelIndex]++;
            }
          }
        }
      }
    }
  }

  // ===== Phase 2: Iteratively flip bad voxels using worklist =====
  // The convergence sweep starts at the maximum possible face-neighbor count (6 in 3D; 2D images
  // simply never reach the top levels because no voxel can have count > 4). Tying this to
  // k_NumFaceNeighbors keeps the upper bound consistent if VoxelNeighbors ever changes.
  constexpr int32 startLevel = static_cast<int32>(k_NumFaceNeighbors);
  const int32 totalLevels = startLevel - m_InputValues->NumberOfNeighbors + 1;

  for(int32 currentLevel = startLevel; currentLevel >= m_InputValues->NumberOfNeighbors; currentLevel--)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    std::deque<usize> worklist;
    for(usize voxelIndex = 0; voxelIndex < totalPoints; voxelIndex++)
    {
      if(neighborCount[voxelIndex] >= currentLevel && !maskCompare->isTrue(voxelIndex))
      {
        worklist.push_back(voxelIndex);
      }
    }

    while(!worklist.empty())
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      throttledMessenger.sendThrottledMessage([&] { return fmt::format("Level '{}' of '{}' || {} voxels queued for processing", (startLevel - currentLevel) + 1, totalLevels, worklist.size()); });

      const usize voxelIndex = worklist.front();
      worklist.pop_front();

      if(maskCompare->isTrue(voxelIndex) || neighborCount[voxelIndex] < currentLevel)
      {
        continue;
      }

      maskCompare->setValue(voxelIndex, true);

      ebsdlib::QuatD quat1(quats[voxelIndex * 4], quats[voxelIndex * 4 + 1], quats[voxelIndex * 4 + 2], quats[voxelIndex * 4 + 3]);
      quat1.positiveOrientation();
      const uint32 laueClassIndex = crystalStructures[cellPhases[voxelIndex]];
      // Defensive: skip voxels with out-of-range Laue index. See matching guard in Phase 1.
      if(laueClassIndex >= numOrientationOps)
      {
        continue;
      }

      const int64 xIdx = static_cast<int64>(voxelIndex) % dims[0];
      const int64 yIdx = (static_cast<int64>(voxelIndex) / dims[0]) % dims[1];
      const int64 zIdx = static_cast<int64>(voxelIndex) / xyStride;

      // "Update Neighbor's Neighbor Count" pass: now that the current voxel just flipped to
      // true, every still-bad face neighbor must have its neighborCount incremented by 1 if
      // its misorientation to the freshly-flipped voxel is within tolerance. Skipping this
      // update would leave the neighbor counts stale and prevent valid cascade flips later.
      const std::array<bool, k_NumFaceNeighbors> isValidFaceNeighbor = computeValidFaceNeighbors(xIdx, yIdx, zIdx, dims);
      for(const auto& faceIndex : faceNeighborInternalIdx)
      {
        if(!isValidFaceNeighbor[faceIndex])
        {
          continue;
        }

        const int64 neighborPoint = static_cast<int64>(voxelIndex) + neighborVoxelIndexOffsets[faceIndex];

        if(!maskCompare->isTrue(neighborPoint))
        {
          if(cellPhases[voxelIndex] == cellPhases[neighborPoint] && cellPhases[voxelIndex] > 0)
          {
            ebsdlib::QuatD quat2(quats[neighborPoint * 4], quats[neighborPoint * 4 + 1], quats[neighborPoint * 4 + 2], quats[neighborPoint * 4 + 3]);
            quat2.positiveOrientation();
            // Quaternion Math is not commutative so do not reorder
            ebsdlib::AxisAngleDType axisAngle = orientationOps[laueClassIndex]->calculateMisorientation(quat1, quat2);
            if(axisAngle[3] < misorientationTolerance)
            {
              neighborCount[neighborPoint]++;
              if(neighborCount[neighborPoint] >= currentLevel)
              {
                worklist.push_back(static_cast<usize>(neighborPoint));
              }
            }
          }
        }
      }
    }
  }

  return {};
}
