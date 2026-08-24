#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Parameters/MultiArraySelectionParameter.hpp"
#include "simplnx/Parameters/StringParameter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT ConvertColorToGrayScaleInputValues
{
  ChoicesParameter::ValueType ConversionAlgorithm;
  VectorFloat32Parameter::ValueType ColorWeights;
  int32 ColorChannel;
  MultiArraySelectionParameter::ValueType InputDataArrayPaths;
  std::vector<DataPath> OutputDataArrayPaths;
  StringParameter::ValueType OutputArrayPrefix;
};

/**
 * @class ConvertColorToGrayScale
 * @brief Converts selected three-component uint8 RGB arrays to single-component grayscale arrays.
 *
 * Concrete in-memory stores use parallel contiguous-pointer conversion, with the abstract
 * datastore implementation retained as a fallback. Out-of-core arrays use bounded chunk
 * buffers and bulk I/O so conversion never performs per-tuple datastore access or allocates
 * working memory proportional to the total tuple count.
 */
class SIMPLNXCORE_EXPORT ConvertColorToGrayScale
{
public:
  ConvertColorToGrayScale(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ConvertColorToGrayScaleInputValues* inputValues);
  ~ConvertColorToGrayScale() noexcept;

  ConvertColorToGrayScale(const ConvertColorToGrayScale&) = delete;
  ConvertColorToGrayScale(ConvertColorToGrayScale&&) noexcept = delete;
  ConvertColorToGrayScale& operator=(const ConvertColorToGrayScale&) = delete;
  ConvertColorToGrayScale& operator=(ConvertColorToGrayScale&&) noexcept = delete;

  using EnumType = uint32_t;
  enum class ConversionType : EnumType
  {
    Luminosity = 0,
    Average = 1,
    Lightness = 2,
    SingleChannel = 3
  };

  Result<> operator()();

  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const ConvertColorToGrayScaleInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
