#pragma once

#include "simplnx/simplnx_export.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Filter/Arguments.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"

#include <vector>

namespace nx::core
{

class IGridGeometry;
template <typename T>
class AbstractDataStore;

namespace segment_features
{
inline constexpr StringLiteral k_6NeighborString = "Face Neighbors";
inline constexpr StringLiteral k_26NeighborString = "All Connected Neighbors";

inline const ChoicesParameter::Choices k_OperationChoices = {k_6NeighborString, k_26NeighborString};

inline constexpr ChoicesParameter::ValueType k_6NeighborIndex = 0ULL;
inline constexpr ChoicesParameter::ValueType k_26NeighborIndex = 1ULL;
} // namespace segment_features

/**
 * @brief Base class for grid segmentation algorithms that share a scanline
 * connected-component-labeling engine.
 *
 * Subclasses provide voxel validity, neighbor similarity, and optional slice
 * preloading. The base engine retains only neighboring label slices in RAM and
 * moves worst-case equivalence/final-label state to temporary record stores
 * when the participating inputs are out-of-core.
 */
class SIMPLNX_EXPORT SegmentFeatures
{

public:
  /** @brief Binds the shared CCL engine to filter-owned data, cancellation, and messaging. */
  SegmentFeatures(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler);

  virtual ~SegmentFeatures();

  SegmentFeatures(const SegmentFeatures&) = delete;            // Copy Constructor Not Implemented
  SegmentFeatures(SegmentFeatures&&) = delete;                 // Move Constructor Not Implemented
  SegmentFeatures& operator=(const SegmentFeatures&) = delete; // Copy Assignment Not Implemented
  SegmentFeatures& operator=(SegmentFeatures&&) = delete;      // Move Assignment Not Implemented

  enum class NeighborScheme : ChoicesParameter::ValueType
  {
    Face = 0,
    FaceEdgeVertex = 1
  };

  /**
   * @brief Segments the grid into features using connected-component labeling.
   *
   * Processes voxels in Z-Y-X scanline order, streaming one Z-slice at a time so
   * that memory use stays proportional to a single slice rather than the whole
   * volume. Subclasses must override isValidVoxel() and areNeighborsSimilar().
   *
   * @param gridGeom The grid geometry providing dimensions and neighbor offsets.
   * @param featureIdsStore The data store to write assigned feature IDs into.
   * @param usesOutOfCoreInput Requires external equivalence scratch when true;
   * false permits the explicit in-memory provider fallback for forced-path testing.
   * @return Result indicating success or an error with a descriptive message.
   */
  Result<> executeCCL(IGridGeometry* gridGeom, AbstractDataStore<int32>& featureIdsStore, bool usesOutOfCoreInput = false);

  /**
   * @brief Applies a random permutation to positive feature IDs after segmentation.
   * @param featureIds Output labels to rewrite; FeatureId zero remains background.
   * @param totalFeatures Number of generated positive features.
   */
  void randomizeFeatureIds(Int32Array* featureIds, uint64 totalFeatures);

  /**
   * @brief The CompareFunctor class is a functor superclass for type-specific
   * scalar comparators. Subclasses override compare() to test whether the data
   * values at two neighboring voxels are similar enough to belong to the same
   * feature; the connected-component labeling algorithm invokes compare()
   * through this interface during neighbor comparison.
   */
  class CompareFunctor
  {
  public:
    virtual ~CompareFunctor() = default;

    /**
     * @brief Compares the data at two voxel indices to decide whether they
     * belong to the same feature.
     * @param index First voxel index
     * @param neighIndex Second (neighbor) voxel index
     * @return true if the two voxels should be in the same feature
     */
    virtual bool compare(int64 index, int64 neighIndex)
    {
      return false;
    }
  };

  /**
   * @brief Can this voxel be a feature member? (mask + phase check, NO featureId check)
   * Default returns true (all voxels are valid).
   * @param point Linear voxel index
   * @return true if this voxel can participate in segmentation
   */
  virtual bool isValidVoxel(int64 point) const;

  /**
   * @brief Should these two adjacent voxels be in the same feature? (data comparison only)
   * Default returns false (no voxels are similar).
   * @param point1 First voxel index
   * @param point2 Second voxel index
   * @return true if the two voxels should be grouped together
   */
  virtual bool areNeighborsSimilar(int64 point1, int64 point2) const;

  /**
   * @brief Called by executeCCL at the start of each Z-slice to allow subclasses
   * to pre-load input data into local buffers, eliminating per-element OOC overhead
   * during neighbor comparisons.
   *
   * Default implementation does nothing.
   * @param iz Current Z-slice index.
   * @param dimX X dimension of the grid.
   * @param dimY Y dimension of the grid.
   * @param dimZ Z dimension of the grid.
   */
  virtual Result<> prepareForSlice(int64 iz, int64 dimX, int64 dimY, int64 dimZ);

protected:
  DataStructure& m_DataStructure;
  bool m_IsPeriodic = false;
  const std::atomic_bool& m_ShouldCancel;
  MessageHelper m_MessageHelper;
  int32 m_FoundFeatures = 0;
  NeighborScheme m_NeighborScheme = NeighborScheme::Face;
};

} // namespace nx::core
