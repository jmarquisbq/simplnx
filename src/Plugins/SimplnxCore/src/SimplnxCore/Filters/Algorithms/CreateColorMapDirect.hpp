#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct CreateColorMapInputValues;

/**
 * @class CreateColorMapDirect
 * @brief Original parallel, direct-store implementation for in-memory arrays.
 *
 * This path preserves the existing per-element access and parallel transformation for
 * contiguous stores, avoiding an in-core regression from staging data through buffers.
 * CreateColorMap dispatches OOC stores to CreateColorMapScanline instead.
 */
class SIMPLNXCORE_EXPORT CreateColorMapDirect
{
public:
  CreateColorMapDirect(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, const CreateColorMapInputValues* inputValues);
  ~CreateColorMapDirect() noexcept;

  CreateColorMapDirect(const CreateColorMapDirect&) = delete;
  CreateColorMapDirect(CreateColorMapDirect&&) noexcept = delete;
  CreateColorMapDirect& operator=(const CreateColorMapDirect&) = delete;
  CreateColorMapDirect& operator=(CreateColorMapDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const CreateColorMapInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
