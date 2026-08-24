#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeLargestCrossSectionsInputValues;

/**
 * @class ComputeLargestCrossSectionsScanline
 * @brief Computes cross sections with bounded plane buffers and bulk store I/O.
 *
 * Disk-backed Feature Ids are never accessed per voxel, and temporary memory is
 * limited to one cross-section rather than the full cell array.
 */
class SIMPLNXCORE_EXPORT ComputeLargestCrossSectionsScanline
{
public:
  ComputeLargestCrossSectionsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                      const ComputeLargestCrossSectionsInputValues* inputValues);
  ~ComputeLargestCrossSectionsScanline() noexcept;

  ComputeLargestCrossSectionsScanline(const ComputeLargestCrossSectionsScanline&) = delete;
  ComputeLargestCrossSectionsScanline(ComputeLargestCrossSectionsScanline&&) noexcept = delete;
  ComputeLargestCrossSectionsScanline& operator=(const ComputeLargestCrossSectionsScanline&) = delete;
  ComputeLargestCrossSectionsScanline& operator=(ComputeLargestCrossSectionsScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeLargestCrossSectionsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
