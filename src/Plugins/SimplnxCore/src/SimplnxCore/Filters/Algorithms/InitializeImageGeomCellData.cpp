#include "InitializeImageGeomCellData.hpp"

#include "simplnx/Common/TypeTraits.hpp"
#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace nx::core;

namespace
{
using RangeType = std::pair<float64, float64>;
constexpr usize k_InitializationChunkValues = 65536;

/** @brief Internal initialization mode used after validating the user-facing choice index. */
enum class InitType : uint64
{
  Manual = 0,
  Random = 1,
  RandomWithRange = 2
};

/** @brief Converts the persisted choice index to an initialization mode and rejects unknown values. */
InitType ConvertIndexToInitType(uint64 index)
{
  switch(index)
  {
  case static_cast<uint64>(InitType::Manual): {
    return InitType::Manual;
  }
  case static_cast<uint64>(InitType::Random): {
    return InitType::Random;
  }
  case static_cast<uint64>(InitType::RandomWithRange): {
    return InitType::RandomWithRange;
  }
  default: {
    throw std::runtime_error("InitializeImageGeomCellData: Invalid value for InitType");
  }
  }
}

/** @brief Creates the seeded integral or floating distribution used by one typed target array. */
template <class T>
auto CreateRandomGenerator(T rangeMin, T rangeMax, uint64 seed)
{
  std::random_device randomDevice;           // Will be used to obtain a seed for the random number engine
  std::mt19937_64 generator(randomDevice()); // Standard mersenne_twister_engine seeded with rd()
  generator.seed(seed);

  if constexpr(std::is_integral_v<T>)
  {
    std::uniform_int_distribution<> distribution(rangeMin, rangeMax);
    return std::make_pair(distribution, generator);
  }
  else if constexpr(std::is_floating_point_v<T>)
  {
    std::uniform_real_distribution<T> distribution(rangeMin, rangeMax);
    return std::make_pair(distribution, generator);
  }
}

/**
 * @brief Initializes one typed image-cell subvolume through bounded contiguous row segments.
 *
 * Values are generated in the original X/Y/Z and component order so seeded
 * random output remains reproducible. Each row segment is written once rather
 * than mutating individual values in a potentially disk-backed store.
 */
struct InitializeArrayFunctor
{
  /** @brief Generates and writes the selected inclusive subvolume for one runtime value type. */
  template <class T>
  Result<> operator()(IDataArray& dataArray, const std::array<usize, 3>& dims, uint64 xMin, uint64 xMax, uint64 yMin, uint64 yMax, uint64 zMin, uint64 zMax, InitType initType, float64 initValue,
                      const RangeType& initRange, uint64 seed, const std::atomic_bool& shouldCancel)
  {
    T rangeMin;
    T rangeMax;
    if(initType == InitType::RandomWithRange)
    {
      rangeMin = static_cast<T>(initRange.first);
      rangeMax = static_cast<T>(initRange.second);
    }
    else
    {
      rangeMin = std::numeric_limits<T>().min();
      rangeMax = std::numeric_limits<T>().max();
    }

    auto& dataStore = dataArray.template getIDataStoreRefAs<AbstractDataStore<T>>();

    auto&& [distribution, generator] = CreateRandomGenerator(rangeMin, rangeMax, seed);
    const usize numComponents = dataStore.getNumberOfComponents();
    if(numComponents == 0)
    {
      return MakeErrorResult(-27490, "InitializeImageGeomCellData cannot initialize an array with zero components.");
    }
    if(dims[0] == 0 || dims[1] == 0 || dims[2] == 0 || dims[1] > std::numeric_limits<usize>::max() / dims[0])
    {
      return MakeErrorResult(-27491, "InitializeImageGeomCellData encountered image dimensions that overflow the data store index type.");
    }
    const usize sliceTupleCount = dims[0] * dims[1];
    const usize tuplesPerChunk = std::max<usize>(1, k_InitializationChunkValues / numComponents);
    auto values = std::make_unique<T[]>(tuplesPerChunk * numComponents);
    const T manualValue = static_cast<T>(initValue);

    for(uint64 k = zMin;; k++)
    {
      for(uint64 j = yMin;; j++)
      {
        for(uint64 i = xMin;;)
        {
          if(shouldCancel)
          {
            return {};
          }

          const usize tupleCount = std::min<usize>(tuplesPerChunk, xMax - i + 1);
          for(usize tupleIndex = 0; tupleIndex < tupleCount; tupleIndex++)
          {
            const T value = initType == InitType::Manual ? manualValue : distribution(generator);
            std::fill_n(values.get() + (tupleIndex * numComponents), numComponents, value);
          }

          if(k > std::numeric_limits<usize>::max() / sliceTupleCount || j > (std::numeric_limits<usize>::max() - (k * sliceTupleCount)) / dims[0] ||
             i > std::numeric_limits<usize>::max() - ((k * sliceTupleCount) + (j * dims[0])))
          {
            return MakeErrorResult(-27491, "InitializeImageGeomCellData encountered image dimensions that overflow the data store index type.");
          }
          const usize tupleIndex = (k * sliceTupleCount) + (j * dims[0]) + i;
          if(tupleIndex > std::numeric_limits<usize>::max() / numComponents)
          {
            return MakeErrorResult(-27492, "InitializeImageGeomCellData encountered an array offset that overflows the data store index type.");
          }
          auto writeResult = dataStore.copyFromBuffer(tupleIndex * numComponents, nonstd::span<const T>(values.get(), tupleCount * numComponents));
          if(writeResult.invalid())
          {
            return writeResult;
          }

          if(tupleCount == xMax - i + 1)
          {
            break;
          }
          i += tupleCount;
        }
        if(j == yMax)
        {
          break;
        }
      }
      if(k == zMax)
      {
        break;
      }
    }

    return {};
  }
};
} // namespace

// -----------------------------------------------------------------------------
InitializeImageGeomCellData::InitializeImageGeomCellData(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                         InitializeImageGeomCellDataInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
InitializeImageGeomCellData::~InitializeImageGeomCellData() noexcept = default;

// -----------------------------------------------------------------------------
Result<> InitializeImageGeomCellData::operator()()
{
  auto cellArrayPaths = m_InputValues->CellArrays;
  auto imageGeomPath = m_InputValues->InputImageGeometryPath;
  auto minPoint = m_InputValues->MinPoint;
  auto maxPoint = m_InputValues->MaxPoint;
  auto initTypeIndex = m_InputValues->InitTypeIndex;
  auto initValue = m_InputValues->InitValue;
  auto initRangeVec = m_InputValues->InitRange;

  auto seed = m_InputValues->SeedValue;
  if(!m_InputValues->UseSeed)
  {
    seed = static_cast<std::mt19937_64::result_type>(std::chrono::steady_clock::now().time_since_epoch().count());
  }

  // Store Seed Value in Top Level Array
  m_DataStructure.getDataRefAs<UInt64Array>(DataPath({m_InputValues->SeedArrayName}))[0] = seed;

  uint64 xMin = minPoint.at(0);
  uint64 yMin = minPoint.at(1);
  uint64 zMin = minPoint.at(2);

  uint64 xMax = maxPoint.at(0);
  uint64 yMax = maxPoint.at(1);
  uint64 zMax = maxPoint.at(2);

  InitType initType = ConvertIndexToInitType(initTypeIndex);
  RangeType initRange = {initRangeVec.at(0), initRangeVec.at(1)};

  const auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(imageGeomPath);

  std::array<usize, 3> dims = imageGeom.getDimensions().toArray();
  std::vector<const IArray*> arrayTargets;
  arrayTargets.reserve(cellArrayPaths.size());
  for(const DataPath& path : cellArrayPaths)
  {
    arrayTargets.push_back(&m_DataStructure.getDataRefAs<IDataArray>(path));
  }

  const AlgorithmArrayTargets dispatchTargets(std::move(arrayTargets));
  const bool usesOutOfCoreStore = AnyOutOfCore(dispatchTargets);
  const bool useOutOfCorePath = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useOutOfCorePath ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);

  for(const DataPath& path : cellArrayPaths)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    auto& iDataArray = m_DataStructure.getDataRefAs<IDataArray>(path);

    auto initializeResult = ExecuteNeighborFunction(InitializeArrayFunctor{}, iDataArray.getDataType(), iDataArray, dims, xMin, xMax, yMin, yMax, zMin, zMax, initType, initValue, initRange, seed,
                                                    m_ShouldCancel); // NO BOOL
    if(initializeResult.invalid())
    {
      return initializeResult;
    }

    // Avoid the exact same seeding for each array
    seed++;
  }

  return {};
}
