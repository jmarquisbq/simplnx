#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT ComputeLargestCrossSectionsInputValues
{
  ChoicesParameter::ValueType Plane;
  DataPath ImageGeometryPath;
  DataPath FeatureIdsArrayPath;
  DataPath LargestCrossSectionsArrayPath;
};

/**
 * @class ComputeLargestCrossSections
 * @brief Computes each feature's largest cross-section perpendicular to a
 * selected image axis by dispatching to storage-appropriate implementations.
 */
class SIMPLNXCORE_EXPORT ComputeLargestCrossSections
{
public:
  ComputeLargestCrossSections(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeLargestCrossSectionsInputValues* inputValues);
  ~ComputeLargestCrossSections() noexcept;

  ComputeLargestCrossSections(const ComputeLargestCrossSections&) = delete;
  ComputeLargestCrossSections(ComputeLargestCrossSections&&) noexcept = delete;
  ComputeLargestCrossSections& operator=(const ComputeLargestCrossSections&) = delete;
  ComputeLargestCrossSections& operator=(ComputeLargestCrossSections&&) noexcept = delete;

  /**
   * @brief Uses direct contiguous access for in-memory arrays and bounded plane
   * bulk reads for out-of-core arrays.
   */
  Result<> operator()();

  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const ComputeLargestCrossSectionsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
