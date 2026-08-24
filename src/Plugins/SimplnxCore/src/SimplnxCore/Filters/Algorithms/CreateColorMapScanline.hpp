#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct CreateColorMapInputValues;

/**
 * @class CreateColorMapScanline
 * @brief Bounded-memory bulk-I/O implementation for out-of-core arrays.
 *
 * The first sequential pass computes the exact typed minimum and maximum. A second
 * pass bulk-reads values and an optional mask, computes colors in a bounded buffer,
 * and bulk-writes RGB output. RAM remains O(chunk), independent of tuple count.
 */
class SIMPLNXCORE_EXPORT CreateColorMapScanline
{
public:
  CreateColorMapScanline(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, const CreateColorMapInputValues* inputValues);
  ~CreateColorMapScanline() noexcept;

  CreateColorMapScanline(const CreateColorMapScanline&) = delete;
  CreateColorMapScanline(CreateColorMapScanline&&) noexcept = delete;
  CreateColorMapScanline& operator=(const CreateColorMapScanline&) = delete;
  CreateColorMapScanline& operator=(CreateColorMapScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const CreateColorMapInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
