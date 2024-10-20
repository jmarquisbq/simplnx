#include "CreateAttributeMatrixFilter.hpp"

#include "simplnx/DataStructure/DataGroup.hpp"
#include "simplnx/Filter/Actions/CreateAttributeMatrixAction.hpp"
#include "simplnx/Parameters/DataGroupCreationParameter.hpp"
#include "simplnx/Parameters/DynamicTableParameter.hpp"
#include "simplnx/Parameters/StringParameter.hpp"
#include "simplnx/Utilities/SIMPLConversion.hpp"

namespace nx::core
{
std::string CreateAttributeMatrixFilter::name() const
{
  return FilterTraits<CreateAttributeMatrixFilter>::name;
}

std::string CreateAttributeMatrixFilter::className() const
{
  return FilterTraits<CreateAttributeMatrixFilter>::className;
}

Uuid CreateAttributeMatrixFilter::uuid() const
{
  return FilterTraits<CreateAttributeMatrixFilter>::uuid;
}

std::string CreateAttributeMatrixFilter::humanName() const
{
  return "Create Attribute Matrix";
}

//------------------------------------------------------------------------------
std::vector<std::string> CreateAttributeMatrixFilter::defaultTags() const
{
  return {className(), "Core", "Generation", "AttributeMatrix", "Create"};
}

Parameters CreateAttributeMatrixFilter::parameters() const
{
  Parameters params;

  params.insertSeparator(Parameters::Separator{"Input Parameter(s)"});
  params.insert(std::make_unique<DataGroupCreationParameter>(k_DataObjectPath, "DataObject Path", "The complete path to the Attribute Matrix being created", DataPath{}));

  DynamicTableInfo tableInfo;
  tableInfo.setRowsInfo(DynamicTableInfo::StaticVectorInfo(1));
  tableInfo.setColsInfo(DynamicTableInfo::DynamicVectorInfo(1, "DIM {}"));
  params.insert(std::make_unique<DynamicTableParameter>(k_TupleDims_Key, "Attribute Matrix Dimensions (Slowest to Fastest Dimensions)", "Slowest to Fastest Dimensions", tableInfo));

  return params;
}

//------------------------------------------------------------------------------
IFilter::VersionType CreateAttributeMatrixFilter::parametersVersion() const
{
  return 1;
}

IFilter::UniquePointer CreateAttributeMatrixFilter::clone() const
{
  return std::make_unique<CreateAttributeMatrixFilter>();
}

IFilter::PreflightResult CreateAttributeMatrixFilter::preflightImpl(const DataStructure& dataStructure, const Arguments& args, const MessageHandler& messageHandler,
                                                                    const std::atomic_bool& shouldCancel) const
{
  auto dataObjectPath = args.value<DataPath>(k_DataObjectPath);
  auto tableData = args.value<DynamicTableParameter::ValueType>(k_TupleDims_Key);

  const auto& rowData = tableData.at(0);
  std::vector<usize> tupleDims;
  tupleDims.reserve(rowData.size());
  for(auto floatValue : rowData)
  {
    tupleDims.push_back(static_cast<usize>(floatValue));
  }

  auto action = std::make_unique<CreateAttributeMatrixAction>(dataObjectPath, tupleDims);

  OutputActions actions;
  actions.appendAction(std::move(action));

  return {std::move(actions)};
}

Result<> CreateAttributeMatrixFilter::executeImpl(DataStructure& dataStructure, const Arguments& args, const PipelineFilter* pipelineNode, const MessageHandler& messageHandler,
                                                  const std::atomic_bool& shouldCancel) const
{
  return {};
}

namespace
{
namespace SIMPL
{
constexpr StringLiteral k_TupleDimensionsKey = "TupleDimensions";
constexpr StringLiteral k_CreatedAttributeMatrixKey = "CreatedAttributeMatrix";
} // namespace SIMPL
} // namespace

Result<Arguments> CreateAttributeMatrixFilter::FromSIMPLJson(const nlohmann::json& json)
{
  Arguments args;

  std::vector<Result<>> results;

  // Attribute matrix type parameter is not applicable in NX
  results.push_back(SIMPLConversion::ConvertParameter<SIMPLConversion::DynamicTableFilterParameterConverter>(args, json, SIMPL::k_TupleDimensionsKey, k_TupleDims_Key));
  results.push_back(SIMPLConversion::ConvertParameter<SIMPLConversion::AttributeMatrixCreationFilterParameterConverter>(args, json, SIMPL::k_CreatedAttributeMatrixKey, k_DataObjectPath));

  Result<> conversionResult = MergeResults(std::move(results));

  return ConvertResultTo<Arguments>(std::move(conversionResult), std::move(args));
}
} // namespace nx::core
