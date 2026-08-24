#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/Geometry/IGeometry.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT PartitionGeometryInputValues
{
  ChoicesParameter::ValueType PartitioningMode;
  int32 StartingFeatureID;
  int32 OutOfBoundsFeatureID;
  VectorInt32Parameter::ValueType NumberOfCellsPerAxis;
  VectorFloat32Parameter::ValueType PartitionGridOrigin;
  VectorFloat32Parameter::ValueType CellLength;
  VectorFloat32Parameter::ValueType MinGridCoord;
  VectorFloat32Parameter::ValueType MaxGridCoord;
  DataPath InputGeomCellAMPath;
  DataPath PartitionGridGeomPath;
  std::string PartitionGridCellAMName;
  std::string PartitionGridFeatureIDsArrayName;
  DataPath InputGeometryToPartition;
  std::string PartitionIdsArrayName;
  DataPath ExistingPartitionGridPath;
  bool UseVertexMask;
  DataPath VertexMaskPath;
  std::string FeatureAttrMatrixName;
};

/**
 * @class PartitionGeometry
 * @brief Dispatches geometry partitioning to an in-memory parallel implementation
 * or a bounded bulk-I/O implementation based on the participating array stores.
 */
class SIMPLNXCORE_EXPORT PartitionGeometry
{
public:
  using VertexStore = AbstractDataStore<IGeometry::SharedVertexList::value_type>;

  /**
   * @brief Constructs the dispatcher with the shared inputs required by both
   * storage-specific implementations.
   */
  PartitionGeometry(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, PartitionGeometryInputValues* inputValues);
  ~PartitionGeometry() noexcept;

  PartitionGeometry(const PartitionGeometry&) = delete;
  PartitionGeometry(PartitionGeometry&&) noexcept = delete;
  PartitionGeometry& operator=(const PartitionGeometry&) = delete;
  PartitionGeometry& operator=(PartitionGeometry&&) noexcept = delete;

  struct PSGeomInfo
  {
    USizeVec3 geometryDims;
    std::optional<FloatVec3> geometryOrigin;
    std::optional<FloatVec3> geometrySpacing;
    IGeometry::LengthUnit geometryUnits;
  };

  /**
   * @brief Selects Direct for in-memory arrays and Scanline when any relevant
   * input/output array is out-of-core or OOC execution is forced by a test.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  PartitionGeometryInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
