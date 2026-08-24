/**
 * @file QuickSurfaceMesh.cpp
 * @brief Dispatcher implementation for the QuickSurfaceMesh algorithm.
 *
 * This file contains the thin dispatch layer that examines the backing
 * storage of the FeatureIds array and forwards execution to either:
 *   - QuickSurfaceMeshDirect  -- when all arrays are in-memory (operator[])
 *   - QuickSurfaceMeshScanline -- when any array uses chunked OOC storage
 *
 * The dispatch decision is made by DispatchAlgorithm, which inspects whether
 * the DataStore is a chunked format. This avoids the 100-1000x performance
 * penalty of random element access through virtual operator[] on OOC stores.
 */

#include "QuickSurfaceMesh.hpp"
#include "QuickSurfaceMeshDirect.hpp"
#include "QuickSurfaceMeshScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/TriangleGeom.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"

using namespace nx::core;

// -----------------------------------------------------------------------------
QuickSurfaceMesh::QuickSurfaceMesh(DataStructure& dataStructure, QuickSurfaceMeshInputValues* inputValues, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
QuickSurfaceMesh::~QuickSurfaceMesh() noexcept = default;

// -----------------------------------------------------------------------------
/**
 * @brief Dispatches to the correct algorithm variant based on DataStore type.
 *
 * The FeatureIds array is the only input array whose access pattern matters
 * for performance: the algorithm reads every voxel and its +X, +Y, +Z
 * neighbors, which is sequential in Z-slice order but random across chunks
 * when using OOC storage. All other arrays (triangle connectivity, vertex
 * coordinates, face labels) are output-only and written sequentially.
 */
Result<> QuickSurfaceMesh::operator()()
{
  std::vector<const IArray*> targets;
  const auto append = [this, &targets](const DataPath& path) {
    if(const auto* array = m_DataStructure.getDataAs<IDataArray>(path); array != nullptr)
    {
      targets.push_back(array);
    }
  };

  append(m_InputValues->FeatureIdsArrayPath);
  append(m_InputValues->NodeTypesDataPath);
  append(m_InputValues->FaceLabelsDataPath);
  for(const auto& path : m_InputValues->SelectedCellDataArrayPaths)
  {
    append(path);
  }
  for(const auto& path : m_InputValues->SelectedFeatureDataArrayPaths)
  {
    append(path);
  }
  for(const auto& path : m_InputValues->CreatedDataArrayPaths)
  {
    append(path);
  }

  const auto& triangleGeom = m_DataStructure.getDataRefAs<TriangleGeom>(m_InputValues->TriangleGeometryPath);
  targets.push_back(triangleGeom.getVertices());
  targets.push_back(triangleGeom.getFaces());
  return DispatchAlgorithm<QuickSurfaceMeshDirect, QuickSurfaceMeshScanline>(AlgorithmArrayTargets(std::move(targets)), m_DataStructure, m_MessageHandler, m_ShouldCancel, m_InputValues);
}
