#pragma once

#include <array>

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"
#include "simplnx/Utilities/SegmentFeatures.hpp"

#include <random>
#include <vector>

namespace nx::core
{

/**
 * @struct ScalarSegmentFeaturesInputValues
 * @brief Holds all user-configured parameters for the ScalarSegmentFeatures algorithm.
 */
struct SIMPLNXCORE_EXPORT ScalarSegmentFeaturesInputValues
{
  int ScalarTolerance = 0;                        ///< Maximum absolute difference between neighboring voxels for grouping.
  bool UseMask;                                   ///< If true, only voxels flagged as "good" in the mask participate.
  bool RandomizeFeatureIds;                       ///< If true, randomize Feature IDs post-segmentation for visual contrast.
  bool IsPeriodic = false;                        ///< If true, treat geometry boundaries as periodic (tileable).
  SegmentFeatures::NeighborScheme NeighborScheme; ///< 6-face or 26-connected neighbor scheme.
  DataPath ImageGeometryPath;                     ///< Path to the ImageGeom / IGridGeometry being segmented.
  DataPath InputDataPath;                         ///< Path to the scalar array used for comparison (any numeric type).
  DataPath MaskArrayPath;                         ///< Path to the boolean/uint8 mask array (used when UseMask is true).
  DataPath FeatureIdsArrayPath;                   ///< Output: per-cell Feature ID array (int32).
  DataPath CellFeatureAttributeMatrixPath;        ///< Output: Attribute Matrix for per-feature arrays.
  DataPath ActiveArrayPath;                       ///< Output: boolean array marking active features.
};

/**
 * @class ScalarSegmentFeatures
 * @brief Segments an ImageGeom into features by grouping contiguous voxels
 * whose scalar values differ by no more than a user-specified tolerance.
 *
 * This is a general-purpose segmentation algorithm that works on any single-component
 * scalar array (int8 through float64, plus boolean), unlike orientation-based
 * segmentation filters (EBSD, CAxis). The tolerance defines the maximum absolute
 * difference between neighboring voxels for them to be grouped into the same feature.
 *
 * @section algorithm Connected-Component Labeling
 * Segmentation is performed by the base-class connected-component labeling
 * algorithm (executeCCL()), which processes data one Z-slice at a time to keep
 * memory usage bounded. The subclass supplies the per-voxel comparison logic by
 * overriding isValidVoxel() and areNeighborsSimilar().
 *
 * @section slice_buffers Slice-Buffer Optimization
 * isValidVoxel() and areNeighborsSimilar() are called for every voxel and its
 * neighbors. To avoid per-element virtual dispatch through the DataStore,
 * prepareForSlice() bulk-reads the scalar input and mask arrays for each Z-slice
 * into contiguous in-memory buffers via copyIntoBuffer(); the override methods
 * then read from these buffers. Two slots are needed because the algorithm
 * compares the current slice with the previous slice (iz and iz-1).
 *
 * All scalar types are converted to float64 in the buffer for uniform comparison,
 * and the tolerance is also cast to float64.
 */
class SIMPLNXCORE_EXPORT ScalarSegmentFeatures : public SegmentFeatures
{
public:
  using FeatureIdsArrayType = Int32Array;
  using GoodVoxelsArrayType = BoolArray;

  /**
   * @brief Creates scalar segmentation and borrows the arrays and execution controls it uses.
   * @param dataStructure Data structure containing the scalar input, optional mask, geometry, and outputs.
   * @param inputValues Non-owning parameter bundle that must remain valid through operator()().
   * @param shouldCancel Cancellation flag checked by the CCL and slice-loading phases.
   * @param mesgHandler Receives connected-component progress messages.
   */
  ScalarSegmentFeatures(DataStructure& dataStructure, ScalarSegmentFeaturesInputValues* inputValues, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler);
  ~ScalarSegmentFeatures() noexcept override;

  ScalarSegmentFeatures(const ScalarSegmentFeatures&) = delete;
  ScalarSegmentFeatures(ScalarSegmentFeatures&&) noexcept = delete;
  ScalarSegmentFeatures& operator=(const ScalarSegmentFeatures&) = delete;
  ScalarSegmentFeatures& operator=(ScalarSegmentFeatures&&) noexcept = delete;

  /**
   * @brief Executes the segmentation: sets up the comparator, runs connected-
   * component labeling, then post-processes (resize AM, fill Active array,
   * optionally randomize IDs).
   * @return Result<> indicating success or error.
   */
  Result<> operator()();

protected:
  /**
   * @brief Checks whether a voxel can participate in segmentation.
   * Uses the slice buffer fast path when available; falls back to direct MaskCompare access.
   * @param point Linear voxel index.
   * @return true if the voxel passes the mask check (or no mask is used).
   */
  bool isValidVoxel(int64 point) const override;

  /**
   * @brief Determines whether two neighboring voxels have similar enough scalar values
   * to belong to the same feature. Uses the slice buffer fast path when
   * both voxels' Z-slices are buffered; falls back to CompareFunctor otherwise.
   * @param point1 First voxel index.
   * @param point2 Second (neighbor) voxel index.
   * @return true if both voxels are valid and their scalar values are within tolerance.
   */
  bool areNeighborsSimilar(int64 point1, int64 point2) const override;

  /**
   * @brief Pre-loads input scalar and mask data for the given Z-slice into the
   * rolling 2-slot buffer, eliminating per-element OOC overhead during CCL.
   *
   * Slot assignment: even slices go to slot 0, odd to slot 1. This ensures that
   * both the current slice and the previous slice are always in memory.
   * Passing iz = -1 disables buffering (used after the slice sweep for Phase 1b).
   *
   * @param iz Current Z-slice index, or -1 to disable buffering.
   * @param dimX X dimension of the grid.
   * @param dimY Y dimension of the grid.
   * @param dimZ Z dimension of the grid.
   */
  Result<> prepareForSlice(int64 iz, int64 dimX, int64 dimY, int64 dimZ) override;

private:
  /**
   * @brief Allocates the rolling 2-slot buffers for scalar and mask data.
   * Each slot holds dimX * dimY elements (one full XY slice).
   * @param dimX X dimension of the grid.
   * @param dimY Y dimension of the grid.
   */
  void allocateSliceBuffers(int64 dimX, int64 dimY);

  /**
   * @brief Releases the slice buffers and resets buffering state.
   */
  void deallocateSliceBuffers();

  const ScalarSegmentFeaturesInputValues* m_InputValues = nullptr;           ///< User-configured parameters.
  FeatureIdsArrayType* m_FeatureIdsArray = nullptr;                          ///< Output Feature IDs array.
  GoodVoxelsArrayType* m_GoodVoxelsArray = nullptr;                          ///< Good voxels mask (if used).
  std::shared_ptr<SegmentFeatures::CompareFunctor> m_CompareFunctor;         ///< Typed comparator used by the CCL neighbor-comparison fallback.
  std::unique_ptr<MaskCompareUtilities::MaskCompare> m_GoodVoxels = nullptr; ///< Mask comparator.
  IDataArray* m_InputDataArray = nullptr;                                    ///< Raw pointer to the input scalar array (for type dispatch).

  // --- Rolling 2-slot input buffers for OOC optimization ---
  std::vector<float64> m_ScalarBuffer;                ///< Scalar values as float64 for uniform comparison (2 * sliceSize elements).
  std::vector<uint8> m_MaskBuffer;                    ///< Mask flags as uint8 (2 * sliceSize elements; 0 = masked out, 1 = valid).
  int64 m_BufSliceSize = 0;                           ///< Number of voxels per XY slice (dimX * dimY).
  std::array<int64, 2> m_BufferedSliceZ = {-1, -1};   ///< Z-index currently loaded in each slot (-1 = empty).
  std::array<uint64, 2> m_BufferUseSequence = {0, 0}; ///< LRU sequence number for each slot.
  uint64 m_NextBufferUseSequence = 1;                 ///< Next sequence number assigned on a prepare call.
  bool m_UseSliceBuffers = false;                     ///< True when the CCL path has activated slice buffering.
};
} // namespace nx::core
