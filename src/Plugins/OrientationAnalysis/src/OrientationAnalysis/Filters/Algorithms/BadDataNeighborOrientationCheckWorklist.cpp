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
 * @brief In-core bad-voxel flipping using two-phase worklist propagation.
 *
 * This algorithm exploits random-access O(1) DataArray subscript access (safe for
 * in-core stores) to achieve O(flipped) amortized cost instead of the O(N * passes)
 * cost of the Scanline variant's full-volume rescans.
 *
 * **Phase 1 -- Initial neighbor counting** (single linear scan, O(N)):
 *   For every bad voxel, iterate over its 6 face-neighbors. For each good neighbor
 *   with the same phase and misorientation within tolerance, increment the voxel's
 *   neighborCount. This produces a baseline count before any flips occur.
 *
 * **Phase 2 -- Worklist-driven propagation** (per level, O(flipped)):
 *   For each level (6 down to NumberOfNeighbors):
 *     1. Seed a deque with all bad voxels whose neighborCount >= currentLevel.
 *     2. Pop the front voxel. If it has already been flipped (by a neighbor cascade)
 *        or its count has dropped below the threshold (impossible in practice but
 *        checked defensively), skip it.
 *     3. Flip the voxel's mask to true.
 *     4. For each still-bad face-neighbor of the newly-flipped voxel: check if the
 *        neighbor has a matching orientation (same phase, misorientation < tolerance).
 *        If so, increment its neighborCount. If the count now meets the threshold,
 *        enqueue the neighbor for processing.
 *     5. Repeat until the deque drains, then move to the next level.
 *
 * This is essentially a breadth-first flood-fill constrained by crystallographic
 * misorientation. The cascade effect means that flipping one voxel can immediately
 * enable its neighbors to flip, propagating outward from high-confidence seeds.
 *
 * **Why this is not suitable for OOC**: The deque pops voxels in arbitrary spatial
 * order (BFS wavefront). Each pop accesses the popped voxel's quaternion, phase,
 * and mask, plus all 6 neighbors' data -- all random-access lookups. On OOC stores,
 * each such lookup could trigger a disk-chunk load/evict, creating catastrophic
 * chunk thrashing for large datasets.
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

  // Precompute the face-neighbor index offsets (-X, +X, -Y, +Y, -Z, +Z) relative
  // to a voxel's linear index in the flat array. These are constant for any given
  // volume geometry.
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
  // This O(N) array is the trade-off: the Worklist variant uses O(N) memory to achieve O(flipped)
  // propagation speed, while the Scanline variant uses O(slice) memory but O(N * passes).
  std::vector<int32> neighborCount(totalPoints, 0);

  MessageHelper messageHelper(m_MessageHandler);
  ThrottledMessenger throttledMessenger = messageHelper.createThrottledMessenger();

  // ===== Phase 1: Count matching good neighbors for each bad voxel =====
  // Single linear scan over all voxels. For each bad voxel, check its 6 face-neighbors
  // for good voxels with matching phase and orientation within tolerance.
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
      // Build the target voxel's quaternion for misorientation comparisons.
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

      // Decompose the linear index into (x, y, z) coordinates for boundary checks.
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

        // Only count good neighbors (mask == true) with the same phase and a
        // misorientation below the tolerance.
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
  // Iterate from the strictest level (all face-neighbors must agree) down to the user's minimum.
  // At each level, seed the worklist with all eligible voxels, then drain it with propagation.
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

    // Seed the worklist with all bad voxels that already meet this level's threshold.
    std::deque<usize> worklist;
    for(usize voxelIndex = 0; voxelIndex < totalPoints; voxelIndex++)
    {
      if(neighborCount[voxelIndex] >= currentLevel && !maskCompare->isTrue(voxelIndex))
      {
        worklist.push_back(voxelIndex);
      }
    }

    // Process the worklist. When a voxel is flipped, its still-bad neighbors may
    // gain a new matching good neighbor and become eligible, creating a cascade.
    while(!worklist.empty())
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize voxelIndex = worklist.front();
      worklist.pop_front();

      // Defensive check: skip if already flipped (by a prior cascade) or if the
      // count dropped below threshold (should not happen, but guards correctness).
      if(maskCompare->isTrue(voxelIndex) || neighborCount[voxelIndex] < currentLevel)
      {
        continue;
      }

      // Flip this voxel from bad to good.
      maskCompare->setValue(voxelIndex, true);

      // Now propagate: for each still-bad face-neighbor, check if the newly-flipped
      // voxel constitutes a new matching good neighbor for that neighbor.
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
              // Increment the neighbor's count because the just-flipped voxel is
              // now a new good neighbor for it.
              neighborCount[neighborPoint]++;
              // If the neighbor now meets the threshold, enqueue it for processing.
              // It may be enqueued multiple times as different neighbors flip, but
              // the defensive check at the top of the while loop handles duplicates.
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
