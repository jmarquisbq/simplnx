#include "ComputeMisorientationsFilter.hpp"
#include "OrientationAnalysis/Filters/Algorithms/ComputeMisorientations.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/NeighborList.hpp"
#include "simplnx/Filter/Actions/CreateArrayAction.hpp"
#include "simplnx/Filter/Actions/CreateNeighborListAction.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/BoolParameter.hpp"
#include "simplnx/Parameters/DataObjectNameParameter.hpp"

#include "simplnx/Utilities/SIMPLConversion.hpp"

#include "simplnx/Parameters/NeighborListSelectionParameter.hpp"

using namespace nx::core;

namespace nx::core
{
//------------------------------------------------------------------------------
std::string ComputeMisorientationsFilter::name() const
{
  return FilterTraits<ComputeMisorientationsFilter>::name.str();
}

//------------------------------------------------------------------------------
std::string ComputeMisorientationsFilter::className() const
{
  return FilterTraits<ComputeMisorientationsFilter>::className;
}

//------------------------------------------------------------------------------
Uuid ComputeMisorientationsFilter::uuid() const
{
  return FilterTraits<ComputeMisorientationsFilter>::uuid;
}

//------------------------------------------------------------------------------
std::string ComputeMisorientationsFilter::humanName() const
{
  return "Compute Feature Neighbor Misorientations";
}

//------------------------------------------------------------------------------
std::vector<std::string> ComputeMisorientationsFilter::defaultTags() const
{
  return {className(), "Statistics", "Crystallography", "Misorientation"};
}

//------------------------------------------------------------------------------
Parameters ComputeMisorientationsFilter::parameters() const
{
  Parameters params;

  // Create the parameter descriptors that are needed for this filter
  params.insertSeparator(Parameters::Separator{"Input Parameter(s)"});
  params.insertLinkableParameter(std::make_unique<BoolParameter>(k_ComputeAvgMisors_Key, "Compute Average Misorientation Per Feature",
                                                                 "Specifies if the average of the misorienations with the neighboring Features should be stored for each Feature", false));

  params.insertSeparator(Parameters::Separator{"Input Feature Data"});
  params.insert(std::make_unique<NeighborListSelectionParameter>(k_NeighborListArrayPath_Key, "Feature Neighbor List", "List of the contiguous neighboring Features for a given Feature",
                                                                 DataPath({"DataContainer", "Feature Data", "NeighborList"}), NeighborListSelectionParameter::AllowedTypes{nx::core::DataType::int32}));
  params.insert(std::make_unique<ArraySelectionParameter>(k_AvgQuatsArrayPath_Key, "Feature Average Quaternions", "Defines the average orientation of the Feature in quaternion representation",
                                                          DataPath({"DataContainer", "Feature Data", "AvgQuats"}), ArraySelectionParameter::AllowedTypes{nx::core::DataType::float32},
                                                          ArraySelectionParameter::AllowedComponentShapes{{4}}));
  params.insert(std::make_unique<ArraySelectionParameter>(k_FeaturePhasesArrayPath_Key, "Feature Phases", "Specifies to which Ensemble each Feature belongs",
                                                          DataPath({"DataContainer", "Feature Data", "Phases"}), ArraySelectionParameter::AllowedTypes{nx::core::DataType::int32},
                                                          ArraySelectionParameter::AllowedComponentShapes{{1}}));

  params.insertSeparator(Parameters::Separator{"Input Ensemble Data"});
  params.insert(std::make_unique<ArraySelectionParameter>(k_CrystalStructuresArrayPath_Key, "Crystal Structures", "Enumeration representing the crystal structure for each Ensemble",
                                                          DataPath({"DataContainer", "Cell Ensemble Data", "CrystalStructures"}), ArraySelectionParameter::AllowedTypes{DataType::uint32},
                                                          ArraySelectionParameter::AllowedComponentShapes{{1}}));

  params.insertSeparator(Parameters::Separator{"Output Feature Data"});
  params.insert(std::make_unique<DataObjectNameParameter>(k_MisorientationListArrayName_Key, "Misorientation List",
                                                          "The name of the data object containing the list of the misorientation angles with the contiguous neighboring Features for a given Feature",
                                                          "MisorientationList"));
  params.insert(std::make_unique<DataObjectNameParameter>(k_AvgMisorientationsArrayName_Key, "Average Misorientations",
                                                          "The name of the array containing the number weighted average of neighbor misorientations.", "AvgMisorientations"));
  // Associate the Linkable Parameter(s) to the children parameters that they control
  params.linkParameters(k_ComputeAvgMisors_Key, k_AvgMisorientationsArrayName_Key, true);

  return params;
}

//------------------------------------------------------------------------------
IFilter::VersionType ComputeMisorientationsFilter::parametersVersion() const
{
  return 1;
}

//------------------------------------------------------------------------------
IFilter::UniquePointer ComputeMisorientationsFilter::clone() const
{
  return std::make_unique<ComputeMisorientationsFilter>();
}

//------------------------------------------------------------------------------
IFilter::PreflightResult ComputeMisorientationsFilter::preflightImpl(const DataStructure& dataStructure, const Arguments& filterArgs, const MessageHandler& messageHandler,
                                                                     const std::atomic_bool& shouldCancel) const
{
  /****************************************************************************
   * Write any preflight sanity checking codes in this function
   ***************************************************************************/

  /**
   * These are the values that were gathered from the UI or the pipeline file or
   * otherwise passed into the filter. These are here for your convenience. If you
   * do not need some of them remove them.
   */
  auto pComputeAvgMisorsValue = filterArgs.value<bool>(k_ComputeAvgMisors_Key);
  auto pNeighborListArrayPathValue = filterArgs.value<DataPath>(k_NeighborListArrayPath_Key);
  auto pAvgQuatsArrayPathValue = filterArgs.value<DataPath>(k_AvgQuatsArrayPath_Key);
  auto pFeaturePhasesArrayPathValue = filterArgs.value<DataPath>(k_FeaturePhasesArrayPath_Key);
  auto pCrystalStructuresArrayPathValue = filterArgs.value<DataPath>(k_CrystalStructuresArrayPath_Key);
  auto cellFeatDataPath = pAvgQuatsArrayPathValue.getParent();
  auto pMisorientationListArrayPath = cellFeatDataPath.createChildPath(filterArgs.value<std::string>(k_MisorientationListArrayName_Key));
  auto pAvgMisorientationsArrayPath = cellFeatDataPath.createChildPath(filterArgs.value<std::string>(k_AvgMisorientationsArrayName_Key));

  PreflightResult preflightResult;

  std::vector<DataPath> dataArrayPaths;

  // Validate the Quats is a 4 component array
  const auto* avgQuats = dataStructure.getDataAs<Float32Array>(pAvgQuatsArrayPathValue);
  if(avgQuats->getNumberOfComponents() != 4)
  {
    return {MakeErrorResult<OutputActions>(-34500, "Input Average Quaternions does not have 4 components.")};
  }

  const auto* featurePhases = dataStructure.getDataAs<Int32Array>(pFeaturePhasesArrayPathValue);

  dataArrayPaths.push_back(pAvgQuatsArrayPathValue);
  dataArrayPaths.push_back(pFeaturePhasesArrayPathValue);
  dataArrayPaths.push_back(pNeighborListArrayPathValue);

  auto tupleValidityCheck = dataStructure.validateNumberOfTuples(dataArrayPaths);
  if(!tupleValidityCheck)
  {
    return {MakeErrorResult<OutputActions>(-34501, fmt::format("The following DataArrays all must have equal number of tuples but this was not satisfied.\n{}", tupleValidityCheck.error()))};
  }

  nx::core::Result<OutputActions> resultOutputActions;

  if(pComputeAvgMisorsValue)
  {
    auto createArrayAction = std::make_unique<CreateArrayAction>(nx::core::DataType::float32, avgQuats->getIDataStore()->getTupleShape(), std::vector<usize>{1}, pAvgMisorientationsArrayPath);
    resultOutputActions.value().appendAction(std::move(createArrayAction));
  }

  // Create the NeighborList array
  auto createArrayAction = std::make_unique<CreateNeighborListAction>(nx::core::DataType::float32, avgQuats->getNumberOfTuples(), pMisorientationListArrayPath);
  resultOutputActions.value().appendAction(std::move(createArrayAction));

  std::vector<PreflightValue> preflightUpdatedValues;

  // Return both the resultOutputActions and the preflightUpdatedValues via std::move()
  return {std::move(resultOutputActions), std::move(preflightUpdatedValues)};
}

//------------------------------------------------------------------------------
Result<> ComputeMisorientationsFilter::executeImpl(DataStructure& dataStructure, const Arguments& filterArgs, const PipelineFilter* pipelineNode, const MessageHandler& messageHandler,
                                                   const std::atomic_bool& shouldCancel) const
{
  ComputeMisorientationsInputValues inputValues;

  inputValues.ComputeAvgMisors = filterArgs.value<bool>(k_ComputeAvgMisors_Key);
  inputValues.NeighborListArrayPath = filterArgs.value<DataPath>(k_NeighborListArrayPath_Key);
  inputValues.AvgQuatsArrayPath = filterArgs.value<DataPath>(k_AvgQuatsArrayPath_Key);
  inputValues.FeaturePhasesArrayPath = filterArgs.value<DataPath>(k_FeaturePhasesArrayPath_Key);
  inputValues.CrystalStructuresArrayPath = filterArgs.value<DataPath>(k_CrystalStructuresArrayPath_Key);
  auto cellFeatDataPath = inputValues.AvgQuatsArrayPath.getParent();
  inputValues.MisorientationListArrayName = cellFeatDataPath.createChildPath(filterArgs.value<std::string>(k_MisorientationListArrayName_Key));
  inputValues.AvgMisorientationsArrayName = cellFeatDataPath.createChildPath(filterArgs.value<std::string>(k_AvgMisorientationsArrayName_Key));

  return ComputeMisorientations(dataStructure, messageHandler, shouldCancel, &inputValues)();
}

namespace
{
namespace SIMPL
{
constexpr StringLiteral k_FindAvgMisorsKey = "FindAvgMisors";
constexpr StringLiteral k_NeighborListArrayPathKey = "NeighborListArrayPath";
constexpr StringLiteral k_AvgQuatsArrayPathKey = "AvgQuatsArrayPath";
constexpr StringLiteral k_FeaturePhasesArrayPathKey = "FeaturePhasesArrayPath";
constexpr StringLiteral k_CrystalStructuresArrayPathKey = "CrystalStructuresArrayPath";
constexpr StringLiteral k_MisorientationListArrayNameKey = "MisorientationListArrayName";
constexpr StringLiteral k_AvgMisorientationsArrayNameKey = "AvgMisorientationsArrayName";
} // namespace SIMPL
} // namespace

Result<Arguments> ComputeMisorientationsFilter::FromSIMPLJson(const nlohmann::json& json)
{
  Arguments args = ComputeMisorientationsFilter().getDefaultArguments();

  std::vector<Result<>> results;

  results.push_back(SIMPLConversion::ConvertParameter<SIMPLConversion::LinkedBooleanFilterParameterConverter>(args, json, SIMPL::k_FindAvgMisorsKey, k_ComputeAvgMisors_Key));
  results.push_back(SIMPLConversion::ConvertParameter<SIMPLConversion::DataArraySelectionFilterParameterConverter>(args, json, SIMPL::k_NeighborListArrayPathKey, k_NeighborListArrayPath_Key));
  results.push_back(SIMPLConversion::ConvertParameter<SIMPLConversion::DataArraySelectionFilterParameterConverter>(args, json, SIMPL::k_AvgQuatsArrayPathKey, k_AvgQuatsArrayPath_Key));
  results.push_back(SIMPLConversion::ConvertParameter<SIMPLConversion::DataArraySelectionFilterParameterConverter>(args, json, SIMPL::k_FeaturePhasesArrayPathKey, k_FeaturePhasesArrayPath_Key));
  results.push_back(
      SIMPLConversion::ConvertParameter<SIMPLConversion::DataArraySelectionFilterParameterConverter>(args, json, SIMPL::k_CrystalStructuresArrayPathKey, k_CrystalStructuresArrayPath_Key));
  results.push_back(
      SIMPLConversion::ConvertParameter<SIMPLConversion::LinkedPathCreationFilterParameterConverter>(args, json, SIMPL::k_MisorientationListArrayNameKey, k_MisorientationListArrayName_Key));
  results.push_back(
      SIMPLConversion::ConvertParameter<SIMPLConversion::LinkedPathCreationFilterParameterConverter>(args, json, SIMPL::k_AvgMisorientationsArrayNameKey, k_AvgMisorientationsArrayName_Key));

  Result<> conversionResult = MergeResults(std::move(results));

  return ConvertResultTo<Arguments>(std::move(conversionResult), std::move(args));
}
} // namespace nx::core
