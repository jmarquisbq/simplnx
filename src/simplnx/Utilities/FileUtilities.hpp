/* ============================================================================
 * Copyright (c) 2020 BlueQuartz Software, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.
 *
 * Neither the names of any of the BlueQuartz Software contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#pragma once

#include "simplnx/Common/Result.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/StringArray.hpp"
#include "simplnx/Parameters/util/ReadCSVData.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/StringInterpretationUtilities.hpp"

#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <type_traits>
#include <vector>

namespace nx::core::FileUtilities
{

/**
 * @brief
 * @param filepath
 * @return
 */
SIMPLNX_EXPORT int64 LinesInFile(const std::string& filepath);

/**
 * @brief
 * @param filePath
 * @return
 */
SIMPLNX_EXPORT Result<> ValidateCSVFile(const std::string& filePath);

/**
 * @brief
 * @param filePath
 * @return
 */
SIMPLNX_EXPORT bool HasWriteAccess(const std::string& path);

/**
 * @brief
 * @param filePath
 * @return
 */
SIMPLNX_EXPORT Result<> ValidateDirectoryWritePermission(const std::filesystem::path& path, bool isFile);

/**
 * @brief
 * @param filePath
 * @return
 */
SIMPLNX_EXPORT std::pair<bool, int32> IsUtf8(const std::filesystem::path& filePath);

namespace CSV
{
/**
 * @brief Parses one CSV column into its destination array and exposes flush
 * operations so buffered numeric data can be committed in bounded bulk writes.
 */
class SIMPLNX_EXPORT AbstractDataParser
{
public:
  virtual ~AbstractDataParser() = default;

  AbstractDataParser(const AbstractDataParser&) = delete;            // Copy Constructor Not Implemented
  AbstractDataParser(AbstractDataParser&&) = delete;                 // Move Constructor Not Implemented
  AbstractDataParser& operator=(const AbstractDataParser&) = delete; // Copy Assignment Not Implemented
  AbstractDataParser& operator=(AbstractDataParser&&) = delete;      // Move Assignment

  [[nodiscard]] std::string columnName() const;

  [[nodiscard]] usize columnIndex() const;

  [[nodiscard]] const IArray& array() const;

  /**
   * @brief Parses a token for the specified destination array index. In-memory
   * arrays are written directly; buffered stores set flushRequired when their
   * fixed-capacity buffer becomes full.
   */
  virtual Result<> parse(const std::string& token, usize index, bool& flushRequired) = 0;

  /**
   * @brief Commits any parsed values that have not yet reached the destination array.
   */
  virtual Result<> flush() = 0;

protected:
  AbstractDataParser(IArray& array, const std::string& columnName, usize columnIndex);

private:
  IArray& m_Array;
  usize m_ColumnIndex = 0;
  std::string m_ColumnName;
};

/**
 * @brief Parses one CSV column while buffering primitive values for contiguous
 * bulk writes. String values retain direct assignment because StringArray has
 * required in-memory storage semantics.
 */
template <typename ArrayType, typename T>
class CSVDataParser : public AbstractDataParser
{
public:
  CSVDataParser(ArrayType& array, const std::string& name, usize index)
  : AbstractDataParser(array, name, index)
  , m_Array(array)
  {
    if constexpr(!std::is_same_v<T, std::string>)
    {
      auto& dataStore = m_Array.getDataStoreRef();
      bool useBufferedPath = !ForceInCoreAlgorithm() && (ForceOocAlgorithm() || dataStore.getStoreType() != IDataStore::StoreType::InMemory);
      if(!useBufferedPath)
      {
        auto* inCoreStore = dynamic_cast<DataStore<T>*>(&dataStore);
        if(inCoreStore != nullptr)
        {
          m_InCoreData = inCoreStore->data();
        }
        else
        {
          useBufferedPath = true;
        }
      }

      const bool usesOutOfCoreStore = dataStore.getStoreType() == IDataStore::StoreType::OutOfCore;
      RecordAlgorithmPathExecution(useBufferedPath ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);
      if(useBufferedPath)
      {
        m_Buffer = std::make_unique<T[]>(k_BufferCapacity);
      }
    }
  }
  ~CSVDataParser() override = default;

  CSVDataParser(const CSVDataParser&) = delete;            // Copy Constructor Not Implemented
  CSVDataParser(CSVDataParser&&) = delete;                 // Move Constructor Not Implemented
  CSVDataParser& operator=(const CSVDataParser&) = delete; // Copy Assignment Not Implemented
  CSVDataParser& operator=(CSVDataParser&&) = delete;      // Move Assignment

  Result<> parse(const std::string& token, usize index, bool& flushRequired) override
  {
    if constexpr(std::is_same_v<T, std::string>)
    {
      const std::regex re(R"(^['"]+|['"]+$)"); // Remove quotes and double quotes
      m_Array[index] = std::regex_replace(token, re, "");
    }
    else
    {
      Result<T> parseResult = StringInterpretationUtilities::Convert<T>(token);
      if(parseResult.invalid())
      {
        return ConvertResult(std::move(parseResult));
      }

      if(m_InCoreData != nullptr)
      {
        m_InCoreData[index] = parseResult.value();
        return {};
      }

      if(m_BufferSize == 0)
      {
        m_BufferStartIndex = index;
      }
      m_Buffer[m_BufferSize] = parseResult.value();
      m_BufferSize++;
      flushRequired |= m_BufferSize == k_BufferCapacity;
    }

    return {};
  }

  Result<> flush() override
  {
    if constexpr(std::is_same_v<T, std::string>)
    {
      return {};
    }
    else
    {
      if(m_BufferSize == 0)
      {
        return {};
      }

      Result<> result = m_Array.getDataStoreRef().copyFromBuffer(m_BufferStartIndex, nonstd::span<const T>(m_Buffer.get(), m_BufferSize));
      if(result.valid())
      {
        m_BufferSize = 0;
      }
      return result;
    }
  }

private:
  static constexpr usize k_BufferCapacity = 65'536;

  ArrayType& m_Array;
  T* m_InCoreData = nullptr;
  std::unique_ptr<T[]> m_Buffer;
  usize m_BufferStartIndex = 0;
  usize m_BufferSize = 0;
};

using Int8Parser = CSVDataParser<Int8Array, int8>;
using UInt8Parser = CSVDataParser<UInt8Array, uint8>;

using Int16Parser = CSVDataParser<Int16Array, int16>;
using UInt16Parser = CSVDataParser<UInt16Array, uint16>;

using Int32Parser = CSVDataParser<Int32Array, int32>;
using UInt32Parser = CSVDataParser<UInt32Array, uint32>;

using Int64Parser = CSVDataParser<Int64Array, int64>;
using UInt64Parser = CSVDataParser<UInt64Array, uint64>;

using Float32Parser = CSVDataParser<Float32Array, float32>;
using Float64Parser = CSVDataParser<Float64Array, float64>;

using BoolParser = CSVDataParser<BoolArray, bool>;

using StringParser = CSVDataParser<StringArray, std::string>;

using ParsersVector = std::vector<std::unique_ptr<AbstractDataParser>>;

/**
 *
 * @param csvTypes
 * @param skippedArrays
 * @param parentPath
 * @param headers
 * @param dataStructure
 * @return
 */
SIMPLNX_EXPORT Result<ParsersVector> CreateParsers(const std::vector<CSVType>& csvTypes, const std::vector<bool>& skippedArrays, const DataPath& parentPath, const std::vector<std::string>& headers,
                                                   DataStructure& dataStructure);

/**
 * @brief Flushes all non-skipped CSV parsers so final partial numeric buffers
 * are committed and every bulk-write failure is returned to the caller.
 */
SIMPLNX_EXPORT Result<> FlushParsers(const ParsersVector& dataParsers);

/**
 *
 * @param inStream
 * @param dataParsers
 * @param headers
 * @param delimiters
 * @param consecutiveDelimiters
 * @param lineNumber
 * @param beginIndex
 * @param flushRequired Set when one or more bounded parser buffers become full.
 * @return
 */
SIMPLNX_EXPORT Result<> ParseLine(std::fstream& inStream, const ParsersVector& dataParsers, const std::vector<std::string>& headers, const std::vector<char>& delimiters, bool consecutiveDelimiters,
                                  usize lineNumber, usize beginIndex, bool& flushRequired);

/**
 *
 * @param tupleDims
 * @return
 */
SIMPLNX_EXPORT std::string TupleDimsToString(const ShapeType& tupleDims);

/**
 *
 * @param headers
 * @return
 */
SIMPLNX_EXPORT std::vector<std::string> RemoveIllegalCharacters(std::vector<std::string>& headers);

/**
 *
 * @param inStream
 * @param numberOfLines
 * @return
 */
SIMPLNX_EXPORT bool SkipNumberOfLines(std::fstream& inStream, usize numberOfLines);

/**
 *
 * @param inputFilePath
 * @param headersLineNum
 * @return
 */
SIMPLNX_EXPORT Result<std::vector<std::string>> ReadHeaders(const std::string& inputFilePath, usize headersLineNum, const std::vector<char>& delimiters, bool consecutiveDelimiters);
} // namespace CSV
} // namespace nx::core::FileUtilities
