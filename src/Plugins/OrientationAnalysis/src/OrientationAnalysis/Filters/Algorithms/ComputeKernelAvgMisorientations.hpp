#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"

#include <chrono>

namespace nx::core
{

/**
 * @brief Input values for the ComputeKernelAvgMisorientations algorithm.
 */
struct ORIENTATIONANALYSIS_EXPORT ComputeKernelAvgMisorientationsInputValues
{
  VectorInt32Parameter::ValueType KernelSize;     ///< Half-widths {kX, kY, kZ} of the kernel in each dimension
  bool UseFeatureIds = true;                      ///< Use same-feature neighbors when true; valid same-phase neighbors when false
  DataPath FeatureIdsArrayPath;                   ///< Cell-level Int32 feature ID per voxel
  DataPath CellPhasesArrayPath;                   ///< Cell-level Int32 phase index per voxel
  DataPath QuatsArrayPath;                        ///< Cell-level Float32 quaternions (4 components)
  DataPath CrystalStructuresArrayPath;            ///< Ensemble-level UInt32 crystal structure Laue classes
  DataPath KernelAverageMisorientationsArrayName; ///< Output: Cell-level Float32 KAM value (degrees)
  DataPath InputImageGeometry;                    ///< ImageGeom providing voxel grid dimensions
};

/**
 * @class ComputeKernelAvgMisorientations
 * @brief Computes the Kernel Average Misorientation (KAM) for each cell of an Image Geometry.
 *
 * For each valid cell (featureId > 0 and phase > 0), the misorientation between the cell and
 * every admitted neighbor in a user-sized kernel is averaged and stored in degrees. Neighbors
 * are admitted per-grain (same feature id, the default) or per-voxel (featureId > 0 and same
 * phase) depending on the UseFeatureIds input.
 *
 * This facade dispatches to the parallel direct traversal for in-memory arrays and to a
 * cache-budgeted scanline implementation for out-of-core arrays.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeKernelAvgMisorientations
{
public:
  ComputeKernelAvgMisorientations(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                  ComputeKernelAvgMisorientationsInputValues* inputValues);
  ~ComputeKernelAvgMisorientations() noexcept;

  ComputeKernelAvgMisorientations(const ComputeKernelAvgMisorientations&) = delete;
  ComputeKernelAvgMisorientations(ComputeKernelAvgMisorientations&&) noexcept = delete;
  ComputeKernelAvgMisorientations& operator=(const ComputeKernelAvgMisorientations&) = delete;
  ComputeKernelAvgMisorientations& operator=(ComputeKernelAvgMisorientations&&) noexcept = delete;

  /**
   * @brief Dispatches and executes the KAM computation.
   * @return Result<> with any errors encountered during execution.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeKernelAvgMisorientationsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
