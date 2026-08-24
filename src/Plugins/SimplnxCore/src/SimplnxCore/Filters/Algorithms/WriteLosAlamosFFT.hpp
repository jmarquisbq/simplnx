#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/FileSystemPathParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT WriteLosAlamosFFTInputValues
{
  FileSystemPathParameter::ValueType OutputFile;
  DataPath FeatureIdsArrayPath;
  DataPath CellEulerAnglesArrayPath;
  DataPath CellPhasesArrayPath;
  DataPath ImageGeomPath;
};

/**
 * @class WriteLosAlamosFFT
 * @brief Writes an eight-column Los Alamos FFT text export from image-cell data.
 *
 * Contiguous arrays are formatted directly to the output stream, while
 * disk-backed arrays are streamed in bounded tuple batches to avoid per-voxel
 * I/O and retain a fixed working set.
 */
class SIMPLNXCORE_EXPORT WriteLosAlamosFFT
{
public:
  WriteLosAlamosFFT(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, WriteLosAlamosFFTInputValues* inputValues);
  ~WriteLosAlamosFFT() noexcept;

  WriteLosAlamosFFT(const WriteLosAlamosFFT&) = delete;
  WriteLosAlamosFFT(WriteLosAlamosFFT&&) noexcept = delete;
  WriteLosAlamosFFT& operator=(const WriteLosAlamosFFT&) = delete;
  WriteLosAlamosFFT& operator=(WriteLosAlamosFFT&&) noexcept = delete;

  Result<> operator()();

  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const WriteLosAlamosFFTInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
