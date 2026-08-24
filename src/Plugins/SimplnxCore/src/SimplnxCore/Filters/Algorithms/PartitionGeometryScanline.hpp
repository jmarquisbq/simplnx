#pragma once

#include "PartitionGeometry.hpp"

namespace nx::core
{

/**
 * @class PartitionGeometryScanline
 * @brief Streams geometry inputs and partition IDs in bounded contiguous chunks
 * so out-of-core execution performs no full-store materialization or per-cell I/O.
 */
class SIMPLNXCORE_EXPORT PartitionGeometryScanline
{
public:
  using VertexStore = PartitionGeometry::VertexStore;

  /**
   * @brief Constructs the bounded streaming implementation for out-of-core data.
   */
  PartitionGeometryScanline(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, PartitionGeometryInputValues* inputValues);
  ~PartitionGeometryScanline() noexcept;

  PartitionGeometryScanline(const PartitionGeometryScanline&) = delete;
  PartitionGeometryScanline(PartitionGeometryScanline&&) noexcept = delete;
  PartitionGeometryScanline& operator=(const PartitionGeometryScanline&) = delete;
  PartitionGeometryScanline& operator=(PartitionGeometryScanline&&) noexcept = delete;

  /**
   * @brief Partitions the geometry using bounded contiguous bulk reads and writes.
   */
  Result<> operator()();

  /**
   * @brief Returns the shared cancellation flag checked between output chunks.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const PartitionGeometryInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;

  /**
   * @brief Streams cell partition IDs in bounded contiguous batches so
   * disk-backed outputs are never materialized in memory.
   */
  Result<> partitionCellBasedGeometry(const IGridGeometry& inputGeometry, Int32AbstractDataStore& partitionIdsStore, const ImageGeom& psImageGeom, int outOfBoundsValue);

  /**
   * @brief Streams vertices, optional masks, and partition IDs in bounded
   * contiguous batches to avoid per-element out-of-core store access.
   */
  Result<> partitionNodeBasedGeometry(const VertexStore& vertexListStore, Int32AbstractDataStore& partitionIdsStore, const ImageGeom& psImageGeom, int outOfBoundsValue,
                                      const std::optional<const BoolArray>& maskArrayOpt);
};

} // namespace nx::core
