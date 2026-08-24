#include "SilhouetteDirect.hpp"

#include "Silhouette.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_set>
#include <vector>

using namespace nx::core;

namespace
{
/**
 * @brief Type-specific implementation of the original resident-array silhouette calculation.
 *
 * The complete per-tuple/per-feature distance table is intentionally retained in
 * this Direct route: it avoids recomputation and is efficient only when the data
 * and workspace fit in memory. The dispatched Scanline route uses tiles instead.
 */
template <typename T>
class SilhouetteTemplate
{
public:
  /** @brief Borrows the typed stores and immutable calculation settings used by operator()(). */
  SilhouetteTemplate(const IDataArray& inputIDataArray, Float64AbstractDataStore& outputDataArray, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& maskDataArray, bool useMask,
                     usize numClusters, const Int32AbstractDataStore& featureIds, ClusterUtilities::DistanceMetric distMetric)
  : m_InputData(inputIDataArray.template getIDataStoreRefAs<AbstractDataStoreT>())
  , m_OutputData(outputDataArray)
  , m_FeatureIds(featureIds)
  , m_Mask(maskDataArray)
  , m_UseMask(useMask)
  , m_NumClusters(numClusters)
  , m_DistMetric(distMetric)
  {
  }

  /**
   * @brief Builds feature counts and the complete distance table, then derives each enabled tuple's score.
   */
  void operator()()
  {
    const usize numTuples = m_InputData.getNumberOfTuples();
    const usize numCompDims = m_InputData.getNumberOfComponents();
    const usize totalClusters = m_NumClusters + 1;
    std::vector<float64> inClusterDist(numTuples, 0.0);
    std::vector<float64> outClusterMinDist(numTuples, 0.0);
    std::vector<float64> numTuplesPerFeature(totalClusters, 0.0);
    std::vector<std::vector<float64>> clusterDist(numTuples, std::vector<float64>(totalClusters, 0.0));

    for(usize i = 0; i < numTuples; i++)
    {
      if(!m_UseMask || m_Mask->isTrue(i))
      {
        numTuplesPerFeature[m_FeatureIds[i]]++;
      }
    }

    for(usize i = 0; i < numTuples; i++)
    {
      if(!m_UseMask || m_Mask->isTrue(i))
      {
        for(usize j = 0; j < numTuples; j++)
        {
          if(!m_UseMask || m_Mask->isTrue(j))
          {
            clusterDist[i][m_FeatureIds[j]] += ClusterUtilities::GetDistance(m_InputData, numCompDims * i, m_InputData, numCompDims * j, numCompDims, m_DistMetric);
          }
        }
      }
    }

    for(usize i = 0; i < numTuples; i++)
    {
      if(!m_UseMask || m_Mask->isTrue(i))
      {
        for(usize j = 1; j < totalClusters; j++)
        {
          clusterDist[i][j] /= numTuplesPerFeature[j];
        }
      }
    }

    for(usize i = 0; i < numTuples; i++)
    {
      if(!m_UseMask || m_Mask->isTrue(i))
      {
        const int32 cluster = m_FeatureIds[i];
        inClusterDist[i] = clusterDist[i][cluster];

        float64 minDist = std::numeric_limits<float64>::max();
        for(usize j = 1; j < totalClusters; j++)
        {
          if(cluster != j)
          {
            const float64 dist = clusterDist[i][j];
            if(dist < minDist)
            {
              minDist = dist;
              outClusterMinDist[i] = dist;
            }
          }
        }
      }
    }

    for(usize i = 0; i < numTuples; i++)
    {
      if(!m_UseMask || m_Mask->isTrue(i))
      {
        m_OutputData[i] = (outClusterMinDist[i] - inClusterDist[i]) / std::max(outClusterMinDist[i], inClusterDist[i]);
      }
      else
      {
        m_OutputData[i] = 0.0;
      }
    }
  }

private:
  using AbstractDataStoreT = AbstractDataStore<T>;
  const AbstractDataStoreT& m_InputData;
  Float64AbstractDataStore& m_OutputData;
  const Int32AbstractDataStore& m_FeatureIds;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask;
  bool m_UseMask = false;
  usize m_NumClusters;
  ClusterUtilities::DistanceMetric m_DistMetric;
};
} // namespace

SilhouetteDirect::SilhouetteDirect(DataStructure& dataStructure, const IFilter::MessageHandler&, const std::atomic_bool&, const SilhouetteInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
{
}

SilhouetteDirect::~SilhouetteDirect() noexcept = default;

Result<> SilhouetteDirect::operator()()
{
  // Discover the number of feature slots before allocating the resident distance table.
  auto& featureIds = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureIdsArrayPath).getDataStoreRef();
  std::unordered_set<int32> uniqueIds;
  for(usize i = 0; i < featureIds.getNumberOfTuples(); i++)
  {
    uniqueIds.insert(featureIds[i]);
  }

  // Instantiate the real mask only when requested; a synthetic cell-sized all-true mask is unnecessary.
  std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare;
  if(m_InputValues->UseMask)
  {
    try
    {
      maskCompare = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
    } catch(const std::out_of_range&)
    {
      return MakeErrorResult(-54080, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->MaskArrayPath.toString()));
    }
  }

  const auto& clusteringArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->ClusteringArrayPath);
  auto& outputStore = m_DataStructure.getDataRefAs<Float64Array>(m_InputValues->SilhouetteArrayPath).getDataStoreRef();
  RunTemplateClass<SilhouetteTemplate, types::NoBooleanType>(clusteringArray.getDataType(), clusteringArray, outputStore, maskCompare, m_InputValues->UseMask, uniqueIds.size(), featureIds,
                                                             m_InputValues->DistanceMetric);
  return {};
}
