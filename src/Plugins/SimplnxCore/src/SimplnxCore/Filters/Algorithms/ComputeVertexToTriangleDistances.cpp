#include "ComputeVertexToTriangleDistances.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataGroup.hpp"
#include "simplnx/DataStructure/Geometry/TriangleGeom.hpp"
#include "simplnx/DataStructure/Geometry/VertexGeom.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"
#include "simplnx/Utilities/RTree.hpp"

#include <algorithm>
#include <array>
#include <numeric>

using namespace nx::core;

namespace
{
using RTreeType = RTree<size_t, float, 3, float>;
using SharedTriListT = AbstractDataStore<IGeometry::SharedTriList::value_type>;
using SharedVertexListT = AbstractDataStore<IGeometry::SharedVertexList::value_type>;

/**
 * @brief Safety cap on the number of times the candidate search box is doubled while looking for at least one
 * hit. Doubling this many times grows the box by a factor of 2^64, so the cap is only ever exhausted for
 * degenerate geometry (e.g. NaN vertex/triangle coordinates) where no finite box can overlap any triangle AABB.
 */
constexpr int32 k_MaxBoxExpansions = 64;

/**
 * @brief Take from https://github.com/embree/embree/blob/master/tutorials/common/math/closest_point.h
 * Which has an apache license.
 * @param p
 * @param a
 * @param b
 * @param c
 * @return
 */
Matrix3X1f closestPointTriangle(const Matrix3X1f& p, const Matrix3X1f& a, const Matrix3X1f& b, const Matrix3X1f& c)
{
  const Matrix3X1f ab = b - a;
  const Matrix3X1f ac = c - a;
  const Matrix3X1f ap = p - a;

  const float d1 = ab.dot(ap); // dot(ab, ap);
  const float d2 = ac.dot(ap); // dot(ac, ap);
  if(d1 <= 0.f && d2 <= 0.f)
  {
    return a;
  }

  const Matrix3X1f bp = p - b;
  const float d3 = ab.dot(bp); // dot(ab, bp);
  const float d4 = ac.dot(bp); // dot(ac, bp);
  if(d3 >= 0.f && d4 <= d3)
  {
    return b;
  }

  const Matrix3X1f cp = p - c;
  const float d5 = ab.dot(cp); // dot(ab, cp);
  const float d6 = ac.dot(cp); // dot(ac, cp);
  if(d6 >= 0.f && d5 <= d6)
  {
    return c;
  }

  const float vc = d1 * d4 - d3 * d2;
  if(vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
  {
    const float v = d1 / (d1 - d3);
    return a + v * ab;
  }

  const float vb = d5 * d2 - d1 * d6;
  if(vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
  {
    const float v = d2 / (d2 - d6);
    return a + v * ac;
  }

  const float va = d3 * d6 - d5 * d4;
  if(va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
  {
    const float v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return b + v * (c - b);
  }

  const float denominator = 1.f / (va + vb + vc);
  const float v = vb * denominator;
  const float w = vc * denominator;
  const Matrix3X1f pointInTriangle = a + v * ab + w * ac;

  return pointInTriangle;
}

float32 PointTriangleDistance(const Matrix3X1f& point, const Matrix3X1f& vert0, const Matrix3X1f& vert1, const Matrix3X1f& vert2, const int64 triangle, const Float64AbstractDataStore& normals)
{

  Matrix3X1f closestPointInTriangle = closestPointTriangle(point, vert0, vert1, vert2);

  auto diffPoint = point - closestPointInTriangle; // Gives a vector pointing from the closest point in triangle to point
  // Only do the dot-product of the vector with itself, so we don't incur the penalty of a square root that we might not need
  float dist = diffPoint.dot(diffPoint);

  Matrix3X1f normal = {static_cast<float32>(normals[3 * triangle + 0]), static_cast<float32>(normals[3 * triangle + 1]), static_cast<float32>(normals[3 * triangle + 2])};

  float32 cosTheta = normal.cosTheta(diffPoint);

  if(cosTheta < 0.0f)
  {
    dist *= -1.0f;
  }

  return dist;
}

/**
 * @brief Runs a fixed-size axis-aligned box query against the RTree and returns every triangle whose stored
 * AABB overlaps the box.
 * @param rtree The RTree indexed by per-triangle AABB.
 * @param center The query point.
 * @param halfExtent Half the side length of the (cubic) query box.
 * @return Triangle indices whose AABB overlaps the box, in RTree traversal order (not sorted).
 */
std::vector<size_t> FindTrianglesWithinBox(const RTreeType& rtree, const Matrix3X1f& center, float32 halfExtent)
{
  std::vector<size_t> candidateIds;
  std::function<bool(size_t)> collect = [&candidateIds](size_t triangleIndex) {
    candidateIds.push_back(triangleIndex);
    return true; // keep going; collect every overlapping triangle
  };

  const std::array<float32, 3> minCorner = {center.getX() - halfExtent, center.getY() - halfExtent, center.getZ() - halfExtent};
  const std::array<float32, 3> maxCorner = {center.getX() + halfExtent, center.getY() + halfExtent, center.getZ() + halfExtent};
  rtree.Search(minCorner.data(), maxCorner.data(), collect);
  return candidateIds;
}

/**
 * @brief Finds an initial, cheap-to-compute set of candidate triangles for a source point by querying the RTree
 * with a real (non-zero-volume) box and doubling that box's half-extent until at least one triangle AABB
 * overlaps it.
 *
 * @note Why this is needed: the RTree indexes triangles by their tight AABB. A source point that lies exactly on
 * (or near) a triangle almost never falls inside that triangle's own tight AABB from a zero-volume query -- the
 * AABB only touches the mesh surface along the triangle itself, not the surrounding space. Expanding a real box
 * around the point instead asks "which triangles could plausibly be nearby", which is a question the RTree can
 * answer by pruning the vast majority of triangles.
 * @note This function only produces a *candidate* set to bound the search, not the final answer -- the true
 * closest triangle can still lie outside this box (its AABB can extend into the box even though its closest
 * point to `center` is farther away than any candidate found here, or vice versa). The exact answer is only
 * guaranteed after the radius-refine query in ComputeVertexToTriangleDistancesImpl::compute().
 * @return Candidate triangle indices in RTree traversal order (not sorted); empty only if the search box could
 * not be grown large enough to overlap any triangle within k_MaxBoxExpansions doublings (degenerate geometry).
 */
std::vector<size_t> FindCandidateTrianglesByExpandingBox(const RTreeType& rtree, const Matrix3X1f& center, float32 initialHalfExtent)
{
  float32 halfExtent = initialHalfExtent;
  for(int32 attempt = 0; attempt < k_MaxBoxExpansions; attempt++)
  {
    std::vector<size_t> candidateIds = FindTrianglesWithinBox(rtree, center, halfExtent);
    if(!candidateIds.empty())
    {
      return candidateIds;
    }
    halfExtent *= 2.0f;
  }
  return {};
}

/**
 * @brief Evaluates PointTriangleDistance for each candidate triangle and keeps the closest one. Candidate ids
 * must be pre-sorted in ascending order so that, when two triangles are exactly equidistant, the strict '<'
 * comparison keeps the first (lowest index) triangle encountered -- matching the tie-breaking behavior of a
 * brute-force scan over triangles 0..N-1 in ascending order.
 * @param candidateIds Triangle indices to test, ascending order.
 * @param point The source vertex position being measured.
 * @param triangleList Triangle vertex-index tuples (3 indices per triangle).
 * @param triangleVertices Triangle mesh vertex positions.
 * @param normals Per-triangle normals, used to sign the returned distance.
 * @param bestSignedSquaredDistance In/out running best (squared distance, sign-flipped when on the back side of
 * the closest triangle's normal); callers should seed this with `std::numeric_limits<float32>::max()`.
 * @param bestTriangleId In/out index of the triangle achieving `bestSignedSquaredDistance`, or -1 if none found.
 */
void EvaluateClosestCandidate(nonstd::span<const size_t> candidateIds, const Matrix3X1f& point, const SharedTriListT& triangleList, const SharedVertexListT& triangleVertices,
                              const Float64AbstractDataStore& normals, float32& bestSignedSquaredDistance, int64& bestTriangleId)
{
  for(const size_t t : candidateIds)
  {
    const auto p = static_cast<int64>(triangleList[t * 3 + 0]);
    const auto q = static_cast<int64>(triangleList[t * 3 + 1]);
    const auto r = static_cast<int64>(triangleList[t * 3 + 2]);
    const Matrix3X1f v0(triangleVertices[p * 3 + 0], triangleVertices[p * 3 + 1], triangleVertices[p * 3 + 2]);
    const Matrix3X1f v1(triangleVertices[q * 3 + 0], triangleVertices[q * 3 + 1], triangleVertices[q * 3 + 2]);
    const Matrix3X1f v2(triangleVertices[r * 3 + 0], triangleVertices[r * 3 + 1], triangleVertices[r * 3 + 2]);

    const float32 d = PointTriangleDistance(point, v0, v1, v2, static_cast<int64>(t), normals);
    if(std::abs(d) < std::abs(bestSignedSquaredDistance))
    {
      bestSignedSquaredDistance = d;
      bestTriangleId = static_cast<int64>(t);
    }
  }
}

class ComputeVertexToTriangleDistancesImpl
{
public:
  ComputeVertexToTriangleDistancesImpl(ComputeVertexToTriangleDistances* filter, const SharedTriListT& triangles, const SharedVertexListT& vertices, SharedVertexListT& sourcePoints,
                                       Float32AbstractDataStore& distances, Int64AbstractDataStore& closestTri, const Float64AbstractDataStore& normals, const RTreeType rtree,
                                       float32 initialSearchHalfExtent, ProgressMessageHelper& progressMessageHelper)
  : m_Filter(filter)
  , m_SharedTriangleList(triangles)
  , m_TriangleVertices(vertices)
  , m_SourcePoints(sourcePoints)
  , m_Distances(distances)
  , m_ClosestTri(closestTri)
  , m_Normals(normals)
  , m_RTree(rtree)
  , m_InitialSearchHalfExtent(initialSearchHalfExtent)
  , m_ProgressMessageHelper(progressMessageHelper)
  {
  }
  virtual ~ComputeVertexToTriangleDistancesImpl() = default;

  void operator()(const Range& range) const
  {
    compute(range.min(), range.max());
  }

  void compute(usize start, usize end) const
  {
    ProgressMessenger progressMessenger = m_ProgressMessageHelper.createProgressMessenger();

    int64 counter = 0;
    auto progIncrement = static_cast<int64>((end - start) / 100);

    const size_t numTuples = m_SharedTriangleList.getNumberOfTuples();
    for(usize v = start; v < end; v++)
    {
      if(m_Filter->getCancel())
      {
        return;
      }

      const Matrix3X1f sourcePoint(m_SourcePoints[3 * v], m_SourcePoints[3 * v + 1], m_SourcePoints[3 * v + 2]);

      float32 bestSignedSquaredDistance = std::numeric_limits<float32>::max();
      int64 bestTriangleId = -1;

      if(numTuples > 0)
      {
        // Step 1: cheaply narrow down candidates by growing a real search box until it hits the mesh surface.
        std::vector<size_t> candidateIds = FindCandidateTrianglesByExpandingBox(m_RTree, sourcePoint, m_InitialSearchHalfExtent);
        if(candidateIds.empty())
        {
          // Degenerate geometry (e.g. NaN coordinates): no finite box overlapped any triangle AABB. Fall back
          // to the exhaustive scan so a distance/closest-triangle is still produced, matching the original
          // filter's behavior for this vertex.
          candidateIds.resize(numTuples);
          std::iota(candidateIds.begin(), candidateIds.end(), size_t{0});
        }
        else
        {
          std::sort(candidateIds.begin(), candidateIds.end());
        }
        EvaluateClosestCandidate(candidateIds, sourcePoint, m_SharedTriangleList, m_TriangleVertices, m_Normals, bestSignedSquaredDistance, bestTriangleId);

        // Step 2: exact radius-refine query. The candidate box from step 1 can miss the true closest triangle
        // (a triangle's AABB can extend into the box even when its closest point is farther away than what was
        // found, and vice versa), so the distance found so far is only an upper bound on the true minimum. Any
        // triangle whose true closest point is within `radius` of sourcePoint must have that closest point (which
        // lies on the triangle, hence inside its AABB) within `radius` of sourcePoint on every axis -- so its AABB
        // is guaranteed to overlap a box of half-extent `radius` centered on sourcePoint. Re-querying with that
        // exact radius therefore guarantees the true closest triangle is among the returned candidates, making
        // this pass exact rather than approximate. Skipped when the running best is already an exact zero
        // distance, since nothing can be closer than that.
        const float32 bestAbsSquaredDistance = std::abs(bestSignedSquaredDistance);
        if(bestTriangleId >= 0 && bestAbsSquaredDistance > 0.0f)
        {
          // Pad the radius slightly so that std::sqrt rounding it down by a rounding ULP can never exclude a
          // triangle whose AABB touches the query box boundary exactly.
          const float32 radius = std::sqrt(bestAbsSquaredDistance) * (1.0f + 1.0e-4f);
          std::vector<size_t> refinedCandidateIds = FindTrianglesWithinBox(m_RTree, sourcePoint, radius);
          std::sort(refinedCandidateIds.begin(), refinedCandidateIds.end());

          bestSignedSquaredDistance = std::numeric_limits<float32>::max();
          bestTriangleId = -1;
          EvaluateClosestCandidate(refinedCandidateIds, sourcePoint, m_SharedTriangleList, m_TriangleVertices, m_Normals, bestSignedSquaredDistance, bestTriangleId);
        }
      }

      if(bestTriangleId >= 0)
      {
        m_Distances[v] = bestSignedSquaredDistance;
        m_ClosestTri[v] = bestTriangleId;
      }

      if(m_Distances[v] >= 0.0f)
      {
        m_Distances[v] = std::sqrt(m_Distances[v]);
      }
      else
      {
        m_Distances[v] *= -1.0f;
        m_Distances[v] = std::sqrt(m_Distances[v]);
        m_Distances[v] *= -1.0f;
      }

      if(counter > progIncrement)
      {
        progressMessenger.sendProgressMessage(counter);
        counter = 0;
      }
      counter++;
    }
    progressMessenger.sendProgressMessage(counter);
  }

private:
  ComputeVertexToTriangleDistances* m_Filter;
  const SharedTriListT& m_SharedTriangleList;
  const SharedVertexListT& m_TriangleVertices;
  SharedVertexListT& m_SourcePoints;
  Float32AbstractDataStore& m_Distances;
  Int64AbstractDataStore& m_ClosestTri;
  const Float64AbstractDataStore& m_Normals;
  const RTreeType m_RTree;
  const float32 m_InitialSearchHalfExtent;
  ProgressMessageHelper& m_ProgressMessageHelper;
};

void GetBoundingBoxAtTri(const SharedTriListT& triList, const SharedVertexListT& vertList, size_t triId, nonstd::span<float> bounds)
{
  size_t v0Index = triList[triId * 3 + 0] * 3;
  size_t v1Index = triList[triId * 3 + 1] * 3;
  size_t v2Index = triList[triId * 3 + 2] * 3;

  auto xMinMax = std::minmax({vertList[v0Index + 0], vertList[v1Index + 0], vertList[v2Index + 0]});
  auto yMinMax = std::minmax({vertList[v0Index + 1], vertList[v1Index + 1], vertList[v2Index + 1]});
  auto zMinMax = std::minmax({vertList[v0Index + 2], vertList[v1Index + 2], vertList[v2Index + 2]});
  bounds[0] = xMinMax.first;
  bounds[1] = yMinMax.first;
  bounds[2] = zMinMax.first;
  bounds[3] = xMinMax.second;
  bounds[4] = yMinMax.second;
  bounds[5] = zMinMax.second;
}
} // namespace

// -----------------------------------------------------------------------------
ComputeVertexToTriangleDistances::ComputeVertexToTriangleDistances(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                                   ComputeVertexToTriangleDistancesInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeVertexToTriangleDistances::~ComputeVertexToTriangleDistances() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& ComputeVertexToTriangleDistances::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> ComputeVertexToTriangleDistances::operator()()
{
  auto& vertexGeom = m_DataStructure.getDataRefAs<VertexGeom>(m_InputValues->VertexDataContainer);
  SharedVertexListT& sourceVertices = vertexGeom.getVertices()->getDataStoreRef();
  usize totalElements = vertexGeom.getNumberOfVertices();

  auto& triangleGeom = m_DataStructure.getDataRefAs<TriangleGeom>(m_InputValues->TriangleDataContainer);
  auto numTris = static_cast<usize>(triangleGeom.getNumberOfFaces());
  const SharedTriListT& triangles = triangleGeom.getFaces()->getDataStoreRef();
  const SharedVertexListT& vertices = triangleGeom.getVertices()->getDataStoreRef();

  RTreeType m_RTree;
  // Populate the RTree, tracking the largest single-triangle AABB extent seen along the way. That extent is a
  // reasonable characteristic size for this mesh, and is used to seed the expanding candidate-box search in
  // ComputeVertexToTriangleDistancesImpl::compute() so the first query is unlikely to need many doublings.
  std::vector<float> triBoundsArray(numTris * 6, 0.0F);
  float32 initialSearchHalfExtent = 0.0f;
  for(size_t triIndex = 0; triIndex < numTris; triIndex++)
  {
    GetBoundingBoxAtTri(triangles, vertices, triIndex, {triBoundsArray.data() + (6 * triIndex), 6});
    m_RTree.Insert(triBoundsArray.data() + (6 * triIndex), triBoundsArray.data() + (6 * triIndex) + 3, triIndex); // Note, all values including zero are fine in this version

    const float32 extentX = triBoundsArray[6 * triIndex + 3] - triBoundsArray[6 * triIndex + 0];
    const float32 extentY = triBoundsArray[6 * triIndex + 4] - triBoundsArray[6 * triIndex + 1];
    const float32 extentZ = triBoundsArray[6 * triIndex + 5] - triBoundsArray[6 * triIndex + 2];
    initialSearchHalfExtent = std::max({initialSearchHalfExtent, extentX, extentY, extentZ});
  }
  if(initialSearchHalfExtent <= 0.0f)
  {
    // Degenerate fallback (e.g. every triangle is a zero-area/coincident-point degenerate triangle) so the
    // expanding search still starts from a non-zero box.
    initialSearchHalfExtent = 1.0f;
  }

  const auto& normalsArray = m_DataStructure.getDataAs<Float64Array>(m_InputValues->TriangleNormalsArrayPath)->getDataStoreRef();
  auto& distancesArray = m_DataStructure.getDataAs<Float32Array>(m_InputValues->DistancesArrayPath)->getDataStoreRef();
  distancesArray.fill(std::numeric_limits<float32>::max());
  auto& closestTriangleIdsArray = m_DataStructure.getDataAs<Int64Array>(m_InputValues->ClosestTriangleIdArrayPath)->getDataStoreRef();
  closestTriangleIdsArray.fill(-1); // -1 means it never found the closest triangle?

  MessageHelper messageHelper(m_MessageHandler);
  ProgressMessageHelper progressMessageHelper = messageHelper.createProgressMessageHelper();
  progressMessageHelper.setMaxProgresss(totalElements);
  progressMessageHelper.setProgressMessageTemplate("Finding Distances || {:.2f}% Completed");

  // Allow data-based parallelization
  ParallelDataAlgorithm dataAlg;
  dataAlg.setParallelizationEnabled(true);
  dataAlg.setRange(0, totalElements);
  dataAlg.execute(
      ComputeVertexToTriangleDistancesImpl(this, triangles, vertices, sourceVertices, distancesArray, closestTriangleIdsArray, normalsArray, m_RTree, initialSearchHalfExtent, progressMessageHelper));

  return {};
}
