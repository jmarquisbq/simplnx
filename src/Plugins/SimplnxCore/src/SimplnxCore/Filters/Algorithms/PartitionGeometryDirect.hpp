#pragma once

#include "PartitionGeometry.hpp"

namespace nx::core
{

/**
 * @class PartitionGeometryDirect
 * @brief Preserves the original parallel implementation for in-memory arrays,
 * avoiding the buffer-copy overhead measured for small in-core geometries.
 */
class SIMPLNXCORE_EXPORT PartitionGeometryDirect
{
public:
  using VertexStore = PartitionGeometry::VertexStore;

  /**
   * @brief Constructs the original parallel implementation for in-memory data.
   */
  PartitionGeometryDirect(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, PartitionGeometryInputValues* inputValues);
  ~PartitionGeometryDirect() noexcept;

  PartitionGeometryDirect(const PartitionGeometryDirect&) = delete;
  PartitionGeometryDirect(PartitionGeometryDirect&&) noexcept = delete;
  PartitionGeometryDirect& operator=(const PartitionGeometryDirect&) = delete;
  PartitionGeometryDirect& operator=(PartitionGeometryDirect&&) noexcept = delete;

  /**
   * @brief Partitions the geometry using direct per-element access and parallel ranges.
   */
  Result<> operator()();

  /**
   * @brief Returns the shared cancellation flag used by the parallel workers.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const PartitionGeometryInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;

  Result<> partitionCellBasedGeometry(const IGridGeometry& inputGeometry, Int32AbstractDataStore& partitionIdsStore, const ImageGeom& psImageGeom, int outOfBoundsValue);
  Result<> partitionNodeBasedGeometry(const VertexStore& vertexListStore, Int32AbstractDataStore& partitionIdsStore, const ImageGeom& psImageGeom, int outOfBoundsValue,
                                      const std::optional<const BoolArray>& maskArrayOpt);
};

} // namespace nx::core
