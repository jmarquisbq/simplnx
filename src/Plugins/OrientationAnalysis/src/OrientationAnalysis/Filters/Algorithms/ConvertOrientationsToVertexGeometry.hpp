#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"

#include <EbsdLib/Orientation/OrientationFwd.hpp>

#include <string>

namespace nx::core
{

/**
 * @brief Input values for the ConvertOrientationsToVertexGeometry algorithm.
 */
struct ORIENTATIONANALYSIS_EXPORT ConvertOrientationsToVertexGeometryInputValues
{
  ebsdlib::orientations::Type InputOrientationType; ///< Enumerated representation of InputOrientationArrayPath (Euler, Quaternion, etc.)
  DataPath InputOrientationArrayPath;               ///< Cell-level float32/float64 input orientation array
  std::vector<DataPath> DataPathCopySources;        ///< Additional cell-level arrays to copy onto the output vertex attribute matrix
  bool ConvertToFundamentalZone;                    ///< If true, rotate each orientation into its Laue-class fundamental zone before projecting
  DataPath CellPhasesArrayPath;                     ///< Cell-level phase id array (indices into CrystalStructuresArrayPath); only read when ConvertToFundamentalZone is true
  DataPath CrystalStructuresArrayPath;              ///< Ensemble-level Laue class per phase; only read when ConvertToFundamentalZone is true
  DataPath OutputVertexGeometryPath;                ///< Path to the VertexGeom created by the filter, one vertex per input orientation tuple
  std::string OutputVertexAttrMatrixName;           ///< Name of the vertex attribute matrix on the output geometry
  std::string OutputSharedVertexListName;           ///< Name of the shared vertex list on the output geometry
};

/**
 * @class ConvertOrientationsToVertexGeometry
 * @brief Converts a cell-level orientation array (any of the 8 EbsdLib representations) to
 *        quaternions, optionally rotates each quaternion into its Laue-class fundamental zone,
 *        and projects the result to stereographic (x, y, z) coordinates that become the vertex
 *        positions of a new VertexGeom.
 *
 * ## OOC Optimization
 *
 * The input orientation array and the Cell Phases array both live on the source
 * Image/RectilinearGrid geometry and can be out-of-core. This algorithm streams both arrays in
 * bounded chunks via copyIntoBuffer()/copyFromBuffer() rather than materializing full-size
 * in-core copies, and caches the small ensemble-level Crystal Structures array locally so the
 * per-tuple fundamental-zone lookup never touches the DataStore. See the .cpp for details.
 */
class ORIENTATIONANALYSIS_EXPORT ConvertOrientationsToVertexGeometry
{
public:
  ConvertOrientationsToVertexGeometry(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                      ConvertOrientationsToVertexGeometryInputValues* inputValues);
  ~ConvertOrientationsToVertexGeometry() noexcept = default;

  ConvertOrientationsToVertexGeometry(const ConvertOrientationsToVertexGeometry&) = delete;
  ConvertOrientationsToVertexGeometry(ConvertOrientationsToVertexGeometry&&) noexcept = delete;
  ConvertOrientationsToVertexGeometry& operator=(const ConvertOrientationsToVertexGeometry&) = delete;
  ConvertOrientationsToVertexGeometry& operator=(ConvertOrientationsToVertexGeometry&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ConvertOrientationsToVertexGeometryInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
