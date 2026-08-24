#include "SampleSurfaceMesh.hpp"

#include "simplnx/Common/BoundingBox.hpp"
#include "simplnx/DataStructure/Geometry/RectGridGeom.hpp"
#include "simplnx/DataStructure/Geometry/TriangleGeom.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/Math/GeometryMath.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <nonstd/span.hpp>

#include <chrono>
#include <memory>

using namespace nx::core;

namespace
{
// A feature's bounding box and the ray-length "radius" derived from it. Both
// values are a pure function of the (mesh-scale) triangle geometry, so they
// are computed once per feature up front and reused for every sample point,
// rather than being recomputed on every point-in-polyhedron test as before.
struct FeatureBoundingVolume
{
  BoundingBox3Df Box;
  float32 Radius = 0.0f;
};

// -----------------------------------------------------------------------------
// Tests every sample point of a single Z-slice against each feature's
// polyhedron, in increasing feature-ID order, and assigns the first (lowest
// ID) feature that contains the point. Operating one slice at a time bounds
// the working set to cellsPerSlice points/outputs, regardless of the total
// size of the sampling grid.
// -----------------------------------------------------------------------------
template <typename OutputT, typename FaceLabelsT>
class SliceSampleSurfaceMeshImpl
{
public:
  SliceSampleSurfaceMeshImpl(const TriangleGeom& faces, const std::vector<std::vector<FaceLabelsT>>& faceLists, const std::vector<BoundingBox3Df>& faceBBs,
                             const std::vector<FeatureBoundingVolume>& featureBounds, const std::vector<Point3Df>& slicePoints, nonstd::span<OutputT> sliceOutput, const std::atomic_bool& shouldCancel,
                             std::atomic_bool& overflowHit)
  : m_Faces(faces)
  , m_FaceLists(faceLists)
  , m_FaceBBs(faceBBs)
  , m_FeatureBounds(featureBounds)
  , m_SlicePoints(slicePoints)
  , m_SliceOutput(sliceOutput)
  , m_ShouldCancel(shouldCancel)
  , m_OverflowHit(overflowHit)
  {
  }
  ~SliceSampleSurfaceMeshImpl() = default;

  SliceSampleSurfaceMeshImpl(const SliceSampleSurfaceMeshImpl&) = default;
  SliceSampleSurfaceMeshImpl(SliceSampleSurfaceMeshImpl&&) noexcept = default;
  SliceSampleSurfaceMeshImpl& operator=(const SliceSampleSurfaceMeshImpl&) = delete;
  SliceSampleSurfaceMeshImpl& operator=(SliceSampleSurfaceMeshImpl&&) = delete;

  void operator()(const Range& range) const
  {
    const usize numFeatures = m_FeatureBounds.size();
    for(usize i = range.min(); i < range.max(); i++)
    {
      // Checked per-point (not just per-range) to match the responsiveness
      // of the previous per-point implementation: each point can perform up
      // to numFeatures ray-cast tests, so the extra atomic read here is
      // negligible relative to the work it can skip.
      if(m_ShouldCancel || m_OverflowHit)
      {
        return;
      }

      const Point3Df point = m_SlicePoints[i];
      OutputT assignedFeature = 0;
      for(usize featureId = 0; featureId < numFeatures; featureId++)
      {
        const FeatureBoundingVolume& featureBounds = m_FeatureBounds[featureId];
        char code = GeometryMath::IsPointInPolyhedron(m_Faces, m_FaceLists[featureId], m_FaceBBs, point, featureBounds.Box, featureBounds.Radius);
        if(code == 'i' || code == 'V' || code == 'E' || code == 'F')
        {
          assignedFeature = static_cast<OutputT>(featureId);
          break;
        }
      }
      m_SliceOutput[i] = assignedFeature;
    }
  }

private:
  const TriangleGeom& m_Faces;
  const std::vector<std::vector<FaceLabelsT>>& m_FaceLists;
  const std::vector<BoundingBox3Df>& m_FaceBBs;
  const std::vector<FeatureBoundingVolume>& m_FeatureBounds;
  const std::vector<Point3Df>& m_SlicePoints;
  nonstd::span<OutputT> m_SliceOutput;
  const std::atomic_bool& m_ShouldCancel;
  std::atomic_bool& m_OverflowHit;
};

// -----------------------------------------------------------------------------
// Drives the Z-slice streaming loop once the output Feature Ids type (OutputT)
// is known: generates one slice of sample points, tests them in parallel
// against every feature, and bulk-writes the slice's results back to the
// output array via copyFromBuffer.
// -----------------------------------------------------------------------------
struct SampleSlicesFunctor
{
  template <typename OutputT, typename FaceLabelsT>
  Result<> operator()(SampleSurfaceMesh* algorithm, const TriangleGeom& triangleGeom, const std::vector<std::vector<FaceLabelsT>>& faceLists, const std::vector<BoundingBox3Df>& faceBBs,
                      const std::vector<FeatureBoundingVolume>& featureBounds, IDataArray& polyIds, const std::atomic_bool& shouldCancel, MessageHelper& messageHelper)
  {
    const usize numFeatures = faceLists.size();

    // An overflow occurs when the Feature ID range (bounded by the Face
    // Labels' integer type) cannot be represented by the narrower output
    // Feature Ids type.
    std::atomic_bool overflowHit(false);
    if constexpr(std::numeric_limits<FaceLabelsT>::max() > std::numeric_limits<OutputT>::max())
    {
      if(std::numeric_limits<OutputT>::max() < numFeatures - 1)
      {
        overflowHit = true;
      }
    }

    auto& outputStore = polyIds.getIDataStoreRefAs<AbstractDataStore<OutputT>>();

    const SizeVec3 gridDims = algorithm->getGridDimensions();
    const usize cellsPerSlice = gridDims.getX() * gridDims.getY();
    const usize numSlices = gridDims.getZ();

    messageHelper.sendMessage("Sampling triangle geometry ...");
    ProgressMessageHelper progressMessageHelper = messageHelper.createProgressMessageHelper();
    progressMessageHelper.setMaxProgresss(numSlices);
    progressMessageHelper.setProgressMessageTemplate("Sampling triangle geometry: {:.1f}%");
    auto progressMessenger = progressMessageHelper.createProgressMessenger(std::chrono::milliseconds(1000));

    // Bounded, per-slice buffers reused across every Z-slice: memory scales
    // with the sampling grid's XY extent only, never with its full volume.
    std::vector<Point3Df> slicePoints(cellsPerSlice);
    auto sliceOutput = std::make_unique<OutputT[]>(cellsPerSlice);

    for(usize zSlice = 0; zSlice < numSlices; zSlice++)
    {
      if(shouldCancel)
      {
        break;
      }

      // Points are generated strictly one slice at a time and in increasing
      // Z order so subclasses drawing from a pseudo-random generator produce
      // the exact same draw sequence as a single monolithic generation pass.
      algorithm->generateSlicePoints(zSlice, slicePoints);

      SliceSampleSurfaceMeshImpl<OutputT, FaceLabelsT> impl(triangleGeom, faceLists, faceBBs, featureBounds, slicePoints, nonstd::span<OutputT>(sliceOutput.get(), cellsPerSlice), shouldCancel,
                                                            overflowHit);
      ParallelDataAlgorithm dataAlg;
      dataAlg.setRange(0, cellsPerSlice);
      dataAlg.execute(impl);

      if(overflowHit || shouldCancel)
      {
        break;
      }

      Result<> copyResult = outputStore.copyFromBuffer(zSlice * cellsPerSlice, nonstd::span<const OutputT>(sliceOutput.get(), cellsPerSlice));
      if(copyResult.invalid())
      {
        return copyResult;
      }

      progressMessenger.sendProgressMessage(1);
    }

    if(overflowHit)
    {
      return MakeErrorResult(
          -158630, fmt::format("Overflow occurred when downcasting a Face Label value of type {} to a feature Id value of type {}. Feature count of {} is greater than max value ({})",
                               DataTypeToHumanString(GetDataType<FaceLabelsT>()), DataTypeToHumanString(polyIds.getDataType()), numFeatures - 1, DataTypeToHumanString(polyIds.getDataType())));
    }

    messageHelper.sendMessage("Complete");

    return {};
  }
};

struct SampleSurfaceMeshFunctor
{
  template <typename T>
  Result<> operator()(SampleSurfaceMesh* algorithm, const TriangleGeom& triangleGeom, const IDataArray& iFaceLabels, IDataArray& polyIds, const std::atomic_bool& shouldCancel,
                      MessageHelper& messageHelper)
  {
    const AbstractDataStore<T>& faceLabelsSM = dynamic_cast<const DataArray<T>&>(iFaceLabels).getDataStoreRef();
    // pull down faces
    const usize numFaces = faceLabelsSM.getNumberOfTuples();

    messageHelper.sendMessage("Counting number of Features...");

    // walk through faces to see how many features there are
    T g1 = 0, g2 = 0;
    T maxFeatureId = 0;
    for(usize i = 0; i < numFaces; i++)
    {
      g1 = faceLabelsSM[2 * i];
      g2 = faceLabelsSM[2 * i + 1];
      if(g1 > maxFeatureId)
      {
        maxFeatureId = g1;
      }
      if(g2 > maxFeatureId)
      {
        maxFeatureId = g2;
      }
    }

    // Check for user canceled flag.
    if(shouldCancel)
    {
      return {};
    }

    // add one to account for feature 0
    usize numFeatures = maxFeatureId + 1;

    std::vector<std::vector<T>> faceLists(numFeatures);
    messageHelper.sendMessage("Counting number of triangle faces per feature ...");

    // traverse data to determine number of faces belonging to each feature
    for(usize i = 0; i < numFaces; i++)
    {
      g1 = faceLabelsSM[2 * i];
      g2 = faceLabelsSM[2 * i + 1];
      if(g1 > 0)
      {
        faceLists[g1].push_back(0);
      }
      if(g2 > 0)
      {
        faceLists[g2].push_back(0);
      }
    }

    // Check for user canceled flag.
    if(shouldCancel)
    {
      return {};
    }

    messageHelper.sendMessage("Allocating triangle faces per feature ...");

    // fill out lists with number of references to cells
    std::vector<int32> linkLoc(numFaces, 0);

    std::vector<BoundingBox3Df> faceBBs;
    {
      // !!! DO NOT USE GeometryStoreCache ELSEWHERE, SPECIAL CASE !!!
      const GeometryMath::detail::GeometryStoreCache cache(triangleGeom.getVertices()->getDataStoreRef(), triangleGeom.getFaces()->getDataStoreRef(), triangleGeom.getNumberOfVerticesPerFace());

      // initialize temp storage 'verts' vector to avoid expensive
      // calls during tight loops below
      std::vector<usize> verts(cache.NumVertsPerFace);

      // traverse data again to get the faces belonging to each feature
      for(int32 i = 0; i < numFaces; i++)
      {
        g1 = faceLabelsSM[2 * i];
        g2 = faceLabelsSM[2 * i + 1];
        if(g1 > 0)
        {
          faceLists[g1][(linkLoc[g1])++] = i;
        }
        if(g2 > 0)
        {
          faceLists[g2][(linkLoc[g2])++] = i;
        }
        // find bounding box for each face
        faceBBs.emplace_back(GeometryMath::FindBoundingBoxOfFace(cache, triangleGeom, i, verts));
      }
    }

    // Check for user canceled flag.
    if(shouldCancel)
    {
      return {};
    }

    // Precompute each feature's bounding box and ray-length radius once: this
    // depends only on the (mesh-scale) triangle geometry, not on the sample
    // points, so computing it up front avoids redundant recomputation for
    // every point tested against a given feature.
    std::vector<FeatureBoundingVolume> featureBounds;
    featureBounds.reserve(numFeatures);
    for(usize featureId = 0; featureId < numFeatures; featureId++)
    {
      BoundingBox3Df boundingBox(GeometryMath::FindBoundingBoxOfFaces(triangleGeom, faceLists[featureId]));
      float32 radius = GeometryMath::FindDistanceBetweenPoints(boundingBox.getMinPoint(), boundingBox.getMaxPoint()) / 2;
      featureBounds.emplace_back(FeatureBoundingVolume{boundingBox, radius});
    }

    // Check for user canceled flag.
    if(shouldCancel)
    {
      return {};
    }

    // Stream the sample-point generation and point-in-polyhedron testing one
    // Z-slice at a time (see SampleSlicesFunctor) instead of materializing
    // every sample point for the whole grid up front.
    return ExecuteDataFunctionIntType(SampleSlicesFunctor{}, polyIds.getDataType(), algorithm, triangleGeom, faceLists, faceBBs, featureBounds, polyIds, shouldCancel, messageHelper);
  }
};
} // namespace

// -----------------------------------------------------------------------------
SampleSurfaceMesh::SampleSurfaceMesh(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler)
: m_DataStructure(dataStructure)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
, m_MessageHelper(m_MessageHandler)
{
}

// -----------------------------------------------------------------------------
SampleSurfaceMesh::~SampleSurfaceMesh() noexcept = default;

// -----------------------------------------------------------------------------
Result<> SampleSurfaceMesh::execute(SampleSurfaceMeshInputValues& inputValues)
{
  auto& triangleGeom = m_DataStructure.getDataRefAs<TriangleGeom>(inputValues.TriangleGeometryPath);
  const auto& iFaceLabels = m_DataStructure.getDataRefAs<IDataArray>(inputValues.SurfaceMeshFaceLabelsArrayPath);

  // create array to hold which polyhedron (feature) each point falls in
  auto& polyIds = m_DataStructure.getDataRefAs<IDataArray>(inputValues.FeatureIdsArrayPath);

  // Face labels are always an integer type (the parameter is restricted to GetIntegerDataTypes()), so dispatch only
  // over the integer types.
  return ExecuteDataFunctionIntType(SampleSurfaceMeshFunctor{}, iFaceLabels.getDataType(), this, triangleGeom, iFaceLabels, polyIds, m_ShouldCancel, m_MessageHelper);
}
