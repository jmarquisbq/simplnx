#include "ReadTextDataArray.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/Parsing/Text/CsvParser.hpp"

#include <filesystem>

namespace fs = std::filesystem;
using namespace nx::core;

namespace
{
/** @brief Dispatches the destination numeric type to the paged CSV parser and checked DataStore writes. */
struct CSVReadFileFunctor
{
  /** @brief Parses typed text values directly into the destination store through CsvParser's bounded pages. */
  template <typename T>
  Result<> operator()(IDataArray* inputIDataArray, const fs::path& inputFilePath, uint64 skipLines, char delimiter)
  {
    auto& store = inputIDataArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    return CsvParser::ReadFile<T>(inputFilePath, store, skipLines, delimiter);
  }
};
} // namespace

// -----------------------------------------------------------------------------
ReadTextDataArray::ReadTextDataArray(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ReadTextDataArrayInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ReadTextDataArray::~ReadTextDataArray() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ReadTextDataArray::operator()()
{
  const auto& inputFilePath = m_InputValues->InputFile;
  auto skipLines = m_InputValues->SkipLineCount;
  auto choiceIndex = m_InputValues->DelimiterIndex;
  const auto& path = m_InputValues->OutputDataArrayPath;

  char delimiter = nx::core::CsvParser::IndexToDelimiter(choiceIndex);

  auto* iDataArray = m_DataStructure.getDataAs<IDataArray>(path);
  return ExecuteDataFunction(CSVReadFileFunctor{}, iDataArray->getDataType(), iDataArray, inputFilePath, skipLines, delimiter);
}
