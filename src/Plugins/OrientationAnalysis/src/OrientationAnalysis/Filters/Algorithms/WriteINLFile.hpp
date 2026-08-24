#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/FileSystemPathParameter.hpp"

namespace nx::core
{

/**
 * @struct WriteINLFileInputValues
 * @brief Collects the output path and input data paths required for an INL export.
 */
struct ORIENTATIONANALYSIS_EXPORT WriteINLFileInputValues
{
  FileSystemPathParameter::ValueType OutputFile;
  DataPath ImageGeomPath;
  DataPath FeatureIdsArrayPath;
  DataPath CellPhasesArrayPath;
  DataPath CellEulerAnglesArrayPath;
  DataPath CrystalStructuresArrayPath;
  DataPath MaterialNameArrayPath;
  DataPath NumFeaturesArrayPath;
};

/**
 * @class WriteINLFile
 * @brief Writes image-cell orientation data to an INL text file.
 *
 * In-memory cell arrays use direct contiguous access. Disk-backed stores are streamed through bounded
 * tuple buffers so they are read sequentially without allocating memory proportional to the image size.
 */
class ORIENTATIONANALYSIS_EXPORT WriteINLFile
{
public:
  /**
   * @brief Constructs the INL writer.
   * @param dataStructure Data structure containing the image and input arrays.
   * @param mesgHandler Message handler used for progress reporting.
   * @param shouldCancel Cancellation flag checked between streamed chunks.
   * @param inputValues Paths and settings used by the export.
   */
  WriteINLFile(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, WriteINLFileInputValues* inputValues);

  /**
   * @brief Destroys the writer.
   */
  ~WriteINLFile() noexcept;

  WriteINLFile(const WriteINLFile&) = delete;
  WriteINLFile(WriteINLFile&&) noexcept = delete;
  WriteINLFile& operator=(const WriteINLFile&) = delete;
  WriteINLFile& operator=(WriteINLFile&&) noexcept = delete;

  /**
   * @brief Writes the INL file using direct in-memory access or bounded disk-backed reads.
   * @return An invalid result if an input bulk read or output write fails.
   */
  Result<> operator()();

  /**
   * @brief Returns the cancellation flag used by the writer.
   * @return The shared cancellation flag.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const WriteINLFileInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
