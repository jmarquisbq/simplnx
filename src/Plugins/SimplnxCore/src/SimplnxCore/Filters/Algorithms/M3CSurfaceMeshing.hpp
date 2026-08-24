#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/Geometry/IGeometry.hpp"
#include "simplnx/DataStructure/IArray.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/MultiArraySelectionParameter.hpp"

#include <vector>

namespace nx::core
{

struct SIMPLNXCORE_EXPORT M3CSurfaceMeshingInputValues
{
  bool RepairTriangleWinding;
  DataPath GridGeomDataPath;
  DataPath FeatureIdsArrayPath;
  MultiArraySelectionParameter::ValueType SelectedCellDataArrayPaths;
  MultiArraySelectionParameter::ValueType SelectedFeatureDataArrayPaths;
  DataPath TriangleGeometryPath;
  DataPath VertexGroupDataPath;
  DataPath NodeTypesDataPath;
  DataPath FaceGroupDataPath;
  DataPath FaceLabelsDataPath;
  MultiArraySelectionParameter::ValueType CreatedDataArrayPaths;
};

/**
 * @class M3CSurfaceMeshing
 * @brief Multi-Material Marching Cubes surface meshing with resident and
 * bounded external-scratch implementations.
 *
 * Port of the legacy DREAM3D `M3CEntireVolume` algorithm (the all-in-memory variant, contributed by
 * Dr. Sukbin Lee, CMU; based on Wu & Sullivan 2003). The slice-by-slice disk round-trip of
 * `M3CSliceBySlice` is intentionally NOT ported. Resident inputs use the
 * optimized sliding-window implementation. When any participating array is
 * disk-backed, a two-pass algorithm stores volume/mesh-scale candidate state
 * in temporary record stores and writes TriangleGeom arrays in bounded batches.
 *
 * Grafted from the slice variant: ghost-layer wrapping and FeatureId==0 renumbering (on a local copy).
 */
class SIMPLNXCORE_EXPORT M3CSurfaceMeshing
{
public:
  using MeshIndexType = IGeometry::MeshIndexType;

  /**
   * @brief Binds the algorithm to filter-owned inputs, outputs, cancellation,
   * and progress reporting. All references are non-owning and must outlive it.
   */
  M3CSurfaceMeshing(DataStructure& dataStructure, M3CSurfaceMeshingInputValues* inputValues, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler);
  ~M3CSurfaceMeshing() noexcept;

  M3CSurfaceMeshing(const M3CSurfaceMeshing&) = delete;
  M3CSurfaceMeshing(M3CSurfaceMeshing&&) noexcept = delete;
  M3CSurfaceMeshing& operator=(const M3CSurfaceMeshing&) = delete;
  M3CSurfaceMeshing& operator=(M3CSurfaceMeshing&&) noexcept = delete;

  /**
   * @brief Inspects every dynamic input/output store and selects the resident
   * sliding-window or bounded external-scratch implementation.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const M3CSurfaceMeshingInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;

  /**
   * @brief Serial reference implementation that allocates all per-site scratch
   * over the entire volume. Retained for validation, not normal dispatch.
   */
  Result<> runEntireVolume();

  /**
   * @brief Sweeps resident inputs by z window so square scratch scales with
   * slice area rather than volume; node types still span the volume.
   * @param parallel False selects the byte-identical serial reference path.
   * True parallelizes independent cubes and is the normal resident path.
   */
  Result<> runWindowed(bool parallel);

  /**
   * @brief Bounded, external-scratch implementation selected when any dynamic
   * cell or mesh target is disk-backed.
   *
   * The implementation is intentionally separate from the legacy pointer-based
   * whole-volume code: it keeps volume and mesh state in temporary record stores
   * and retains only fixed input/output pages in RAM. Genuine OOC execution
   * fails if the external scratch capabilities are unavailable.
   * @param dispatchTargets Participating arrays already used to establish residency.
   * @param usesOutOfCoreStore True when dispatch was caused by an actual disk-backed store.
   * @return The first storage, topology, or output error; cancellation returns success early.
   */
  Result<> runOutOfCore(const std::vector<const IArray*>& dispatchTargets, bool usesOutOfCoreStore);
};
} // namespace nx::core
