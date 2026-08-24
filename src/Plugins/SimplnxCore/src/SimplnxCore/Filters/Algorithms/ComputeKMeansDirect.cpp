#include "ComputeKMeansDirect.hpp"

#include "ComputeKMeans.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"

#include <random>

using namespace nx::core;

// =============================================================================
// ComputeKMeansDirect — In-Core Algorithm
//
// This file implements the in-core (Direct) variant of ComputeKMeans.
// It is selected by DispatchAlgorithm when all input arrays reside in memory.
//
// ALGORITHM OVERVIEW (Lloyd's Algorithm):
//   1. Randomly select k initial centroids from masked data points
//   2. Assign each point to the nearest centroid (findClusters)
//   3. Recompute each centroid as the arithmetic mean of its assigned members
//      (findMeans)
//   4. Repeat steps 2-3 until the centroids stop moving (convergence)
//
// DATA ACCESS PATTERN:
//   Uses operator[] for per-element random access to the input array, means
//   array, and featureIds array. This is optimal for in-memory DataStore where
//   operator[] is essentially a pointer dereference. For out-of-core data, this
//   pattern would cause chunk thrashing — see ComputeKMeansScanline instead.
//
// COMPLEXITY:
//   findClusters: O(n * k * d) per iteration
//   findMeans:    O(n * d) per iteration, but rescans the full input array and
//                 featureIds array once per component (d full passes)
//   Total:        O(iter * n * k * d)
// =============================================================================

namespace
{
/** @brief Constant mask adapter used when K-Means masking is disabled. */
class AllTrueMaskCompare final : public MaskCompareUtilities::MaskCompare
{
public:
  /** @brief Records the real tuple count without allocating a synthetic mask array. */
  explicit AllTrueMaskCompare(usize tupleCount)
  : m_TupleCount(tupleCount)
  {
  }

  /** @brief Reports both tuples as enabled. */
  bool bothTrue(usize, usize) const override
  {
    return true;
  }
  /** @brief No tuple pair is disabled. */
  bool bothFalse(usize, usize) const override
  {
    return false;
  }
  /** @brief Reports every tuple as enabled. */
  bool isTrue(usize) const override
  {
    return true;
  }
  /** @brief No-op because the adapter has no writable backing array. */
  void setValue(usize, bool) override
  {
  }
  /** @brief Returns the input array's tuple count. */
  usize getNumberOfTuples() const override
  {
    return m_TupleCount;
  }
  /** @brief A logical mask has one component. */
  usize getNumberOfComponents() const override
  {
    return 1;
  }
  /** @brief Returns the tuple count because every tuple is enabled. */
  usize countTrueValues() const override
  {
    return m_TupleCount;
  }

private:
  usize m_TupleCount = 0;
};

/**
 * @brief Type-specialized template that performs the actual K-Means computation
 * for the in-core (Direct) path.
 *
 * @tparam T The element type of the clustering array (e.g., float32, int32)
 */
template <typename T>
class ComputeKMeansTemplate
{
public:
  /** @brief Borrows resident typed stores, mask state, K-Means settings, and deterministic seed. */
  ComputeKMeansTemplate(ComputeKMeansDirect* filter, const IDataArray* inputIDataArray, IDataArray* meansIDataArray, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& maskDataArray,
                        usize numClusters, Int32AbstractDataStore& fIds, ClusterUtilities::DistanceMetric distMetric, std::mt19937_64::result_type seed)
  : m_Filter(filter)
  , m_InputArray(inputIDataArray->template getIDataStoreRefAs<AbstractDataStoreT>())
  , m_Means(meansIDataArray->template getIDataStoreRefAs<AbstractDataStoreT>())
  , m_Mask(maskDataArray)
  , m_NumClusters(numClusters)
  , m_FeatureIds(fIds)
  , m_DistMetric(distMetric)
  , m_Seed(seed)
  {
  }
  ~ComputeKMeansTemplate() = default;

  ComputeKMeansTemplate(const ComputeKMeansTemplate&) = delete; // Copy Constructor Not Implemented
  void operator=(const ComputeKMeansTemplate&) = delete;        // Move assignment Not Implemented

  // -----------------------------------------------------------------------------
  /**
   * @brief Main K-Means loop: initialize centroids, then iterate findClusters +
   * findMeans until convergence (mean values stop changing).
   */
  void operator()()
  {
    usize numTuples = m_InputArray.getNumberOfTuples();
    int32 numCompDims = m_InputArray.getNumberOfComponents();

    const usize rangeMax = numTuples - 1;

    std::mt19937_64 gen(m_Seed);
    std::uniform_real_distribution<float64> dist(0.0, 1.0);

    std::vector<usize> clusterIdxs(m_NumClusters);

    usize clusterChoices = 0;
    while(clusterChoices < m_NumClusters)
    {
      usize index = std::floor(dist(gen) * static_cast<float64>(rangeMax));
      if(m_Mask->isTrue(index))
      {
        clusterIdxs[clusterChoices] = index;
        clusterChoices++;
      }
    }

    for(usize i = 0; i < m_NumClusters; i++)
    {
      for(int32 j = 0; j < numCompDims; j++)
      {
        m_Means[numCompDims * (i + 1) + j] = m_InputArray[numCompDims * clusterIdxs[i] + j];
      }
    }

    std::vector<float64> oldMeans(m_NumClusters);
    std::vector<float64> differences(m_NumClusters);
    usize iteration = 1;
    usize updateCheck = 0;
    while(updateCheck != m_NumClusters)
    {
      if(m_Filter->getCancel())
      {
        return;
      }
      findClusters(numTuples, numCompDims);

      for(usize i = 0; i < m_NumClusters; i++)
      {
        oldMeans[i] = m_Means[i + 1];
      }

      findMeans(numTuples, numCompDims);

      updateCheck = 0;
      for(usize i = 0; i < m_NumClusters; i++)
      {
        differences[i] = oldMeans[i] - m_Means[i + 1];
        if(closeEnough<float64>(differences[i], 0.0))
        {
          updateCheck++;
        }
      }

      float64 sum = std::accumulate(std::begin(differences), std::end(differences), 0.0);
      m_Filter->updateProgress(fmt::format("Clustering Data || Iteration {} || Total Mean Shift: {}", iteration, sum));
      iteration++;
    }
  }

private:
  using AbstractDataStoreT = AbstractDataStore<T>;
  ComputeKMeansDirect* m_Filter;
  const AbstractDataStoreT& m_InputArray;
  AbstractDataStoreT& m_Means;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask;
  usize m_NumClusters;
  Int32AbstractDataStore& m_FeatureIds;
  ClusterUtilities::DistanceMetric m_DistMetric;
  std::mt19937_64::result_type m_Seed;

  // -----------------------------------------------------------------------------
  /** @brief Tests whether the centroid shift is within the selected floating-point tolerance. */
  template <typename K>
  bool closeEnough(const K& a, const K& b, const K& epsilon = std::numeric_limits<K>::epsilon())
  {
    return (epsilon > fabs(a - b));
  }

  // -----------------------------------------------------------------------------
  /**
   * @brief Assigns each data point to the nearest centroid using direct operator[] access.
   *
   * For each masked data point, computes the distance to all k centroids and assigns
   * the point to the cluster of the nearest centroid. Uses direct per-element access
   * via operator[] — optimal for in-memory data but would cause chunk thrashing for OOC.
   *
   * @param tuples Total number of tuples in the input array
   * @param dims Number of components per tuple
   */
  void findClusters(usize tuples, int32 dims)
  {
    for(usize i = 0; i < tuples; i++)
    {
      if(m_Filter->getCancel())
      {
        return;
      }
      if(m_Mask->isTrue(i))
      {
        float64 minDist = std::numeric_limits<float64>::max();
        for(int32 j = 0; j < m_NumClusters; j++)
        {
          float64 dist = ClusterUtilities::GetDistance(m_InputArray, (dims * i), m_Means, (dims * (j + 1)), dims, m_DistMetric);
          if(dist < minDist)
          {
            minDist = dist;
            m_FeatureIds[i] = j + 1;
          }
        }
      }
    }
  }

  // -----------------------------------------------------------------------------
  /**
   * @brief Recomputes each cluster's centroid as the arithmetic mean of its assigned
   * members, using direct operator[] access.
   *
   * Note that every tuple (masked or not) contributes to its current featureIds bucket
   * (bucket 0 collects points that findClusters never assigned because they are masked
   * out), matching the original algorithm's behavior exactly.
   *
   * Rescans the full input array and featureIds array once per component (dims passes)
   * because each accumulator update must go through the DataStore's own +=/-= dispatch,
   * which operates one component at a time. This is inexpensive for in-memory data but
   * would multiply chunk reads by dims for out-of-core data — see ComputeKMeansScanline
   * for the single-pass alternative.
   *
   * @param tuples Total number of tuples in the input array
   * @param dims Number of components per tuple
   */
  void findMeans(usize tuples, int32 dims)
  {
    std::vector<usize> counts(m_NumClusters + 1, 0);

    for(usize i = 0; i <= m_NumClusters; i++)
    {
      for(usize j = 0; j < dims; j++)
      {
        m_Means[dims * i + j] = 0.0;
      }
    }

    for(usize i = 0; i < dims; i++)
    {
      for(usize j = 0; j < tuples; j++)
      {
        int32 feature = m_FeatureIds[j];
        m_Means[dims * feature + i] += static_cast<float64>(m_InputArray[dims * j + i]);
        counts[feature] += 1;
      }
      for(usize j = 0; j <= m_NumClusters; j++)
      {
        if(counts[j] == 0)
        {
          m_Means[dims * j + i] = 0.0;
        }
        else
        {
          m_Means[dims * j + i] /= static_cast<float64>(counts[j]);
        }
      }
      std::fill(std::begin(counts), std::end(counts), 0);
    }
  }
};
} // namespace

// -----------------------------------------------------------------------------
ComputeKMeansDirect::ComputeKMeansDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeKMeansInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeKMeansDirect::~ComputeKMeansDirect() noexcept = default;

// -----------------------------------------------------------------------------
void ComputeKMeansDirect::updateProgress(const std::string& message)
{
  m_MessageHandler(IFilter::Message::Type::Info, message);
}

// -----------------------------------------------------------------------------
const std::atomic_bool& ComputeKMeansDirect::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> ComputeKMeansDirect::operator()()
{
  auto* clusteringArray = m_DataStructure.getDataAs<IDataArray>(m_InputValues->ClusteringArrayPath);

  std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare;
  if(m_InputValues->UseMask)
  {
    try
    {
      maskCompare = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
    } catch(const std::exception&)
    {
      return MakeErrorResult(-54060, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->MaskArrayPath.toString()));
    }
  }
  else
  {
    maskCompare = std::make_unique<AllTrueMaskCompare>(clusteringArray->getNumberOfTuples());
  }

  if(maskCompare->countTrueValues() == 0)
  {
    return MakeErrorResult(-54063, "Compute K Means cannot initialize clusters because the mask contains no selected tuples.");
  }

  RunTemplateClass<ComputeKMeansTemplate, types::NoBooleanType>(clusteringArray->getDataType(), this, clusteringArray, m_DataStructure.getDataAs<IDataArray>(m_InputValues->MeansArrayPath),
                                                                maskCompare, m_InputValues->InitClusters, m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath)->getDataStoreRef(),
                                                                m_InputValues->DistanceMetric, m_InputValues->Seed);

  return {};
}
