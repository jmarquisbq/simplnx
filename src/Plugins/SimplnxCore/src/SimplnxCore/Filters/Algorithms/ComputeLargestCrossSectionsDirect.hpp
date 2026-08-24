#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeLargestCrossSectionsInputValues;

/**
 * @class ComputeLargestCrossSectionsDirect
 * @brief Computes cross sections directly from contiguous in-memory Feature Ids.
 *
 * XY traverses contiguous slices, XZ planes are computed independently in parallel
 * with thread-local feature scratch and serial max reduction, and bounded YZ-plane
 * blocks reuse each cache line. Scratch remains proportional to feature count.
 */
class SIMPLNXCORE_EXPORT ComputeLargestCrossSectionsDirect
{
public:
  ComputeLargestCrossSectionsDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                    const ComputeLargestCrossSectionsInputValues* inputValues);
  ~ComputeLargestCrossSectionsDirect() noexcept;

  ComputeLargestCrossSectionsDirect(const ComputeLargestCrossSectionsDirect&) = delete;
  ComputeLargestCrossSectionsDirect(ComputeLargestCrossSectionsDirect&&) noexcept = delete;
  ComputeLargestCrossSectionsDirect& operator=(const ComputeLargestCrossSectionsDirect&) = delete;
  ComputeLargestCrossSectionsDirect& operator=(ComputeLargestCrossSectionsDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeLargestCrossSectionsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
