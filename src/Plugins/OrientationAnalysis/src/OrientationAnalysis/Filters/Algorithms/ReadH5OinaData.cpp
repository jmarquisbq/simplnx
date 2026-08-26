#include "ReadH5OinaData.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"

#include <memory>

using namespace nx::core;

namespace
{

/**
 * @brief Copies one EbsdLib-owned OINA channel in bounded tuple pages.
 * @tparam T Channel value type.
 * @param inputValues Identifies the destination attribute matrix.
 * @param tupleCount Number of tuples in the loaded scan.
 * @param dataStructure Contains the destination array.
 * @param reader Owns the loaded source channel.
 * @param name Channel and destination-array name.
 * @param tupleOffset First destination tuple for this scan.
 * @param shouldCancel Signals cancellation between pages.
 * @return Source or destination errors. Cancellation returns success after completed pages.
 * @pre The source and destination contain tupleCount times their component count.
 *
 * EbsdLib owns one scan in memory. simplnx creates no second scan-sized array.
 */
template <typename T>
Result<> copyRawData(const ReadH5DataInputValues* inputValues, usize tupleCount, DataStructure& dataStructure, ebsdlib::H5OINAReader& reader, const std::string& name, usize tupleOffset,
                     const std::atomic_bool& shouldCancel)
{
  using ArrayType = DataArray<T>;
  auto& dataRef = dataStructure.getDataRefAs<ArrayType>(inputValues->CellAttributeMatrixPath.createChildPath(name));
  const usize components = dataRef.getNumberOfComponents();
  const auto* raw = reinterpret_cast<T*>(reader.getPointerByName(name));
  constexpr usize kTuplesPerBatch = 65536;
  for(usize localOffset = 0; localOffset < tupleCount; localOffset += kTuplesPerBatch)
  {
    if(shouldCancel)
      return {};
    const usize count = std::min(kTuplesPerBatch, tupleCount - localOffset);
    auto result = dataRef.getDataStoreRef().copyFromBuffer((tupleOffset + localOffset) * components, nonstd::span<const T>(raw + localOffset * components, count * components));
    if(result.invalid())
      return result;
  }
  return {};
}

/**
 * @brief Applies the configured 30-degree hexagonal Euler correction in bounded pages.
 * @tparam T Cell phase value type.
 * @param inputValues Identifies the correction option and arrays.
 * @param tupleCount Number of tuples in this scan.
 * @param tupleOffset First volume tuple for this scan.
 * @param dataStructure Contains phase, Euler, and crystal-structure arrays.
 * @param shouldCancel Signals cancellation between pages.
 * @return Source or destination transfer errors. Cancellation returns success after completed pages.
 *
 * The ensemble crystal table is cached because it is small and reused for every
 * cell. Cell phases and Euler triples remain bounded. Phase IDs outside the
 * ensemble range do not receive the correction.
 */
template <typename T>
Result<> convertHexEulerAngle(const ReadH5DataInputValues* inputValues, usize tupleCount, usize tupleOffset, DataStructure& dataStructure, const std::atomic_bool& shouldCancel)
{
  if(inputValues->EdaxHexagonalAlignment)
  {
    auto& crystalStructuresRef = dataStructure.getDataRefAs<UInt32Array>(inputValues->CellEnsembleAttributeMatrixPath.createChildPath(ebsdlib::AngFile::CrystalStructures));
    auto& crystalStructuresDSRef = crystalStructuresRef.getDataStoreRef();
    std::vector<uint32> crystalStructures(crystalStructuresDSRef.getNumberOfTuples());
    auto result = crystalStructuresDSRef.copyIntoBuffer(0, nonstd::span<uint32>(crystalStructures.data(), crystalStructures.size()));
    if(result.invalid())
      return result;
    auto& cellPhasesRef = dataStructure.getDataRefAs<DataArray<T>>(inputValues->CellAttributeMatrixPath.createChildPath(ebsdlib::H5OINA::Phase));
    auto& cellPhasesDSRef = cellPhasesRef.getDataStoreRef();
    auto& eulerRef = dataStructure.getDataRefAs<Float32Array>(inputValues->CellAttributeMatrixPath.createChildPath(ebsdlib::H5OINA::Euler));
    auto& eulerDataStoreRef = eulerRef.getDataStoreRef();
    constexpr usize kTuplesPerBatch = 65536;
    // Keep the bounded pages resident in RAM without consuming the limited Windows thread stack.
    auto phases = std::make_unique<T[]>(kTuplesPerBatch);
    auto eulers = std::make_unique<float32[]>(kTuplesPerBatch * 3);
    for(usize local = 0; local < tupleCount; local += kTuplesPerBatch)
    {
      if(shouldCancel)
        return {};
      const usize count = std::min(kTuplesPerBatch, tupleCount - local);
      result = cellPhasesDSRef.copyIntoBuffer(tupleOffset + local, nonstd::span<T>(phases.get(), count));
      if(result.invalid())
        return result;
      result = eulerDataStoreRef.copyIntoBuffer((tupleOffset + local) * 3, nonstd::span<float32>(eulers.get(), count * 3));
      if(result.invalid())
        return result;
      for(usize i = 0; i < count; i++)
      {
        const usize phase = static_cast<usize>(phases[i]);
        if(phase < crystalStructures.size() && crystalStructures[phase] == ebsdlib::CrystalStructure::Hexagonal_High)
          eulers[i * 3 + 2] += 30.0F;
      }
      result = eulerDataStoreRef.copyFromBuffer((tupleOffset + local) * 3, nonstd::span<const float32>(eulers.get(), count * 3));
      if(result.invalid())
        return result;
    }
  }
  return {};
}

} // namespace

ReadH5OinaData::ReadH5OinaData(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ReadH5DataInputValues* inputValues)
: IEbsdOemReader<ebsdlib::H5OINAReader>(dataStructure, mesgHandler, shouldCancel, inputValues)
{
}

ReadH5OinaData::~ReadH5OinaData() noexcept = default;

Result<> ReadH5OinaData::operator()()
{
  return execute();
}

Result<> ReadH5OinaData::copyRawEbsdData(int index)
{
  const auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeometryPath);
  const usize totalPoints = imageGeom.getNumXCells() * imageGeom.getNumYCells();
  const usize offset = index * totalPoints;

  const auto copy = [this, totalPoints, offset](auto typeTag, const std::string& name) {
    return copyRawData<decltype(typeTag)>(m_InputValues, totalPoints, m_DataStructure, *m_Reader, name, offset, m_ShouldCancel);
  };
  Result<> result = copy(uint8{}, ebsdlib::H5OINA::BandContrast);
  if(result.invalid())
    return result;
  result = copy(uint8{}, ebsdlib::H5OINA::BandSlope);
  if(result.invalid())
    return result;
  result = copy(uint8{}, ebsdlib::H5OINA::Bands);
  if(result.invalid())
    return result;
  result = copy(uint8{}, ebsdlib::H5OINA::Error);
  if(result.invalid())
    return result;
  result = copy(float32{}, ebsdlib::H5OINA::Euler);
  if(result.invalid())
    return result;
  result = copy(float32{}, ebsdlib::H5OINA::MeanAngularDeviation);
  if(result.invalid())
    return result;
  if(m_InputValues->ConvertPhaseToInt32)
  {
    const nonstd::span<uint8> rawDataPtr(reinterpret_cast<uint8*>(m_Reader->getPointerByName(ebsdlib::H5OINA::Phase)), totalPoints);
    using ArrayType = DataArray<int32>;
    auto& dataRef = m_DataStructure.getDataRefAs<ArrayType>(m_InputValues->CellAttributeMatrixPath.createChildPath(ebsdlib::H5OINA::Phase));
    constexpr usize kTuplesPerBatch = 65536;
    // Keep the conversion page in RAM without growing the caller's stack frame.
    auto phaseBuffer = std::make_unique<int32[]>(kTuplesPerBatch);
    for(usize tupleOffset = 0; tupleOffset < totalPoints; tupleOffset += kTuplesPerBatch)
    {
      if(m_ShouldCancel)
        return {};
      const usize count = std::min(kTuplesPerBatch, totalPoints - tupleOffset);
      for(usize i = 0; i < count; i++)
        phaseBuffer[i] = static_cast<int32>(rawDataPtr[tupleOffset + i]);
      result = dataRef.getDataStoreRef().copyFromBuffer(offset + tupleOffset, nonstd::span<const int32>(phaseBuffer.get(), count));
      if(result.invalid())
        return result;
    }
  }
  else
  {
    result = copy(uint8{}, ebsdlib::H5OINA::Phase);
    if(result.invalid())
      return result;
  }
  result = copy(float32{}, ebsdlib::H5OINA::X);
  if(result.invalid())
    return result;
  result = copy(float32{}, ebsdlib::H5OINA::Y);
  if(result.invalid())
    return result;

  if(m_InputValues->EdaxHexagonalAlignment)
  {
    if(m_InputValues->ConvertPhaseToInt32)
    {
      result = convertHexEulerAngle<int32>(m_InputValues, totalPoints, offset, m_DataStructure, m_ShouldCancel);
    }
    else
    {
      result = convertHexEulerAngle<uint8>(m_InputValues, totalPoints, offset, m_DataStructure, m_ShouldCancel);
    }
    if(result.invalid())
      return result;
  }

  if(m_InputValues->ReadPatternData)
  {
    const uint16* patternDataPtr = m_Reader->getPatternData();
    if(patternDataPtr == nullptr)
    {
      return MakeErrorResult(-34970, "Pattern data was requested but no pattern data was found in the data file");
    }
    std::array<int32, 2> pDims = {{0, 0}};
    m_Reader->getPatternDims(pDims);
    if(pDims[0] != 0 && pDims[1] != 0)
    {
      std::vector<usize> pDimsV(2);
      pDimsV[0] = pDims[0];
      pDimsV[1] = pDims[1];
      auto& patternData = m_DataStructure.getDataRefAs<UInt16Array>(m_InputValues->CellAttributeMatrixPath.createChildPath(ebsdlib::H5OINA::UnprocessedPatterns));
      const usize numComponents = patternData.getNumberOfComponents();
      constexpr usize kTuplesPerBatch = 65536;
      for(usize tupleOffset = 0; tupleOffset < totalPoints; tupleOffset += kTuplesPerBatch)
      {
        if(m_ShouldCancel)
          return {};
        const usize count = std::min(kTuplesPerBatch, totalPoints - tupleOffset);
        auto patternResult =
            patternData.getDataStoreRef().copyFromBuffer((offset + tupleOffset) * numComponents, nonstd::span<const uint16>(patternDataPtr + tupleOffset * numComponents, count * numComponents));
        if(patternResult.invalid())
          return patternResult;
      }
    }
  }

  return {};
}
