#pragma once

#include "simplnx/Common/Range.hpp"
#include "simplnx/Common/Result.hpp"
#include "simplnx/DataStructure/Geometry/IGeometry.hpp"
#include "simplnx/DataStructure/Geometry/INodeGeometry2D.hpp"
#include "simplnx/Filter/IFilter.hpp"

/**
 * @namespace nx::core::MeshingUtilities
 * @brief Provides triangle-mesh winding, normal, and volume utilities.
 */
namespace nx::core::MeshingUtilities
{
/**
 * @namespace detail
 * @brief Provides internal triangle measure helpers.
 */
namespace detail
{
inline static constexpr usize k_00 = 0;
inline static constexpr usize k_01 = 1;
inline static constexpr usize k_02 = 2;
inline static constexpr usize k_10 = 3;
inline static constexpr usize k_11 = 4;
inline static constexpr usize k_12 = 5;
inline static constexpr usize k_20 = 6;
inline static constexpr usize k_21 = 7;
inline static constexpr usize k_22 = 8;

/**
 * @brief Calculates one triangle's signed origin-based volume contribution.
 * @param vertIndices Specifies three vertex indexes.
 * @param vertices Provides flat XYZ coordinates.
 * @return Signed volume contribution.
 */
SIMPLNX_EXPORT INodeGeometry2D::SharedVertexList::value_type FindTriangleVolume(const std::array<usize, 3>& vertIndices, const INodeGeometry2D::SharedVertexList::store_type& vertices);
} // namespace detail

/**
 * @brief Makes triangle winding as consistent as mesh topology permits.
 * @param triangles Provides and receives triangle connectivity.
 * @param neighbors Provides adjacent triangles.
 * @param idsStore Provides two face labels or one region ID per triangle.
 * @param shouldCancel Stops before later traversal work when true.
 * @param mesgHandler Receives progress messages.
 * @return Error, warning for unrepaired triangles, or success after cancellation.
 * @pre The mesh has no duplicate vertices.
 */
SIMPLNX_EXPORT Result<> RepairTriangleWinding(INodeGeometry2D::SharedFaceList::store_type& triangles, const DynamicListArray<uint16, IGeometry::MeshIndexType>& neighbors,
                                              const Int32AbstractDataStore& idsStore, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler);

/**
 * @brief Attempts to make triangle winding consistent without materializing mesh-sized face, label,
 * connectivity, traversal-state, or queue arrays in memory.
 *
 * This storage-neutral variant reconstructs the same triangle-neighbor order as the legacy
 * connectivity path through bounded external sorts. Mutable traversal state and the FIFO queue are
 * held in temporary record stores, while face and ID DataStores are accessed through bounded page
 * caches. A registered I/O manager that provides external sorting and temporary record storage is
 * required.
 *
 * @param triangles The SharedFaceList that may be modified.
 * @param idsStore Face labels (2 components) or region IDs (1 component).
 * @param shouldCancel Cooperative cancellation flag.
 * @param mesgHandler Progress-message callback.
 * @return Provider, sort, cache, topology, or DataStore error, or success after cancellation.
 */
SIMPLNX_EXPORT Result<> RepairTriangleWindingExternal(INodeGeometry2D::SharedFaceList::store_type& triangles, const Int32AbstractDataStore& idsStore, const std::atomic_bool& shouldCancel,
                                                      const IFilter::MessageHandler& mesgHandler);

/**
 * @class CalculateNormalsImpl
 * @brief Computes triangle normals over scheduler ranges.
 */
class SIMPLNX_EXPORT CalculateNormalsImpl
{
public:
  /**
   * @brief Creates a borrowed normal-calculation worker.
   * @param triangles Provides triangle connectivity.
   * @param verts Provides vertex coordinates.
   * @param normals Receives three values per triangle.
   * @param shouldCancel Stops before later triangles when true.
   */
  CalculateNormalsImpl(const INodeGeometry2D::SharedFaceList::store_type& triangles, const INodeGeometry2D::SharedVertexList::store_type& verts, Float64AbstractDataStore& normals,
                       const std::atomic_bool& shouldCancel);
  /**
   * @brief Destroys the borrowed worker.
   */
  ~CalculateNormalsImpl() = default;

  /**
   * @brief Computes normals for one triangle range.
   * @param start Specifies the first triangle.
   * @param end Specifies the exclusive last triangle.
   */
  void generate(usize start, usize end) const;

  /**
   * @brief Computes normals for one scheduler range.
   * @param range Specifies the triangle range.
   */
  void operator()(const Range& range) const;

private:
  const INodeGeometry2D::SharedFaceList::store_type& m_Triangles;
  const INodeGeometry2D::SharedVertexList::store_type& m_Vertices;
  Float64AbstractDataStore& m_Normals;
  const std::atomic_bool& m_ShouldCancel;
};

/**
 * @brief Accumulates signed triangle volume contributions by feature ID.
 * @tparam ContainerT Specifies a random-access volume container.
 * @param triangles Provides triangle connectivity.
 * @param verts Provides vertex coordinates.
 * @param idsStore Provides two face labels or one region ID per triangle.
 * @param volumes Receives accumulated volumes.
 * @param shouldCancel Stops before later triangles when true.
 * @return Error for an invalid ID component count, or success after cancellation.
 * @pre volumes contains every nonnegative ID when idsStore has one component.
 *
 * The method uses direct per-value DataStore access and does not report I/O errors.
 */
template <class ContainerT>
Result<> CalculateFeatureVolumes(const INodeGeometry2D::SharedFaceList::store_type& triangles, const INodeGeometry2D::SharedVertexList::store_type& verts, const Int32AbstractDataStore& idsStore,
                                 ContainerT& volumes, const std::atomic_bool& shouldCancel)
{
  usize volumeSize = volumes.size();
  std::array<usize, 3> faceVertexIndices = {0, 0, 0};
  if(idsStore.getNumberOfComponents() == 2)
  {
    for(usize i = 0; i < triangles.getNumberOfTuples(); i++)
    {
      if(shouldCancel)
      {
        return {};
      }

      const usize triangleIndex = i * 3;
      faceVertexIndices[0] = triangles[triangleIndex];
      faceVertexIndices[1] = triangles[triangleIndex + 1];
      faceVertexIndices[2] = triangles[triangleIndex + 2];

      int32 faceLabel0 = idsStore[2 * i + 0];
      int32 faceLabel1 = idsStore[2 * i + 1];

      bool faceLabel0InRange = faceLabel0 >= 0 && faceLabel0 < static_cast<int64_t>(volumeSize);
      bool faceLabel1InRange = faceLabel1 >= 0 && faceLabel1 < static_cast<int64_t>(volumeSize);

      if(faceLabel0 < 0 && faceLabel1InRange)
      {
        std::swap(faceVertexIndices[2], faceVertexIndices[1]);
        volumes[faceLabel1] += detail::FindTriangleVolume(faceVertexIndices, verts);
      }
      else if(faceLabel1 < 0 && faceLabel0InRange)
      {
        volumes[faceLabel0] += detail::FindTriangleVolume(faceVertexIndices, verts);
      }
      else if(faceLabel0InRange && faceLabel1InRange)
      {
        volumes[faceLabel0] += detail::FindTriangleVolume(faceVertexIndices, verts);
        std::swap(faceVertexIndices[2], faceVertexIndices[1]);
        volumes[faceLabel1] += detail::FindTriangleVolume(faceVertexIndices, verts);
      }
    }
  }
  else if(idsStore.getNumberOfComponents() == 1)
  {
    for(usize i = 0; i < triangles.getNumberOfTuples(); i++)
    {
      if(shouldCancel)
      {
        return {};
      }

      const usize triangleIndex = i * 3;
      faceVertexIndices[0] = triangles[triangleIndex];
      faceVertexIndices[1] = triangles[triangleIndex + 2];
      faceVertexIndices[2] = triangles[triangleIndex + 1];

      int32 featureId = idsStore[i];
      if(featureId < 0)
      {
        continue;
      }
      volumes[featureId] += detail::FindTriangleVolume(faceVertexIndices, verts);
    }
  }
  else
  {
    return MakeErrorResult(-65771, fmt::format("MeshingUtilities::CalculateFeatureVolumes: invalid ID array supplied. The ID array must have 1 or 2 components, supplied array components: {}.",
                                               idsStore.getNumberOfComponents()));
  }

  return {};
}
} // namespace nx::core::MeshingUtilities
