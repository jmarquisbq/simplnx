#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/FileSystemPathParameter.hpp"
#include "simplnx/Parameters/NumberParameter.hpp"
#include "simplnx/Parameters/StringParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT WriteAbaqusHexahedronInputValues
{
  int32 HourglassStiffness;
  StringParameter::ValueType JobName;
  FileSystemPathParameter::ValueType OutputPath;
  StringParameter::ValueType FilePrefix;
  DataPath FeatureIdsArrayPath;
  DataPath ImageGeometryPath;
  bool WriteDummyNode;
};

/**
 * @class WriteAbaqusHexahedron
 * @brief Writes an ImageGeom as Abaqus nodes, hexahedral elements, grain
 * element sets, sections, and a master include file.
 *
 * The resident ELSET path groups cells in one pass. Disk-backed FeatureIds use
 * an external sort so all elements for a grain remain contiguous in the file
 * without retaining cell-scale buckets in RAM.
 */
class SIMPLNXCORE_EXPORT WriteAbaqusHexahedron
{
public:
  /** @brief Binds filter-owned geometry, options, progress, and cancellation. */
  WriteAbaqusHexahedron(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, WriteAbaqusHexahedronInputValues* inputValues);
  ~WriteAbaqusHexahedron() noexcept;

  WriteAbaqusHexahedron(const WriteAbaqusHexahedron&) = delete;
  WriteAbaqusHexahedron(WriteAbaqusHexahedron&&) noexcept = delete;
  WriteAbaqusHexahedron& operator=(const WriteAbaqusHexahedron&) = delete;
  WriteAbaqusHexahedron& operator=(WriteAbaqusHexahedron&&) noexcept = delete;

  /** @brief Creates all five temporary output files and atomically commits them on success. */
  Result<> operator()();

  /** @brief Returns the filter-owned cancellation flag. */
  const std::atomic_bool& getCancel();

  /** @brief Forwards a progress message to the filter's message handler. */
  void sendMessage(const std::string& message);

private:
  DataStructure& m_DataStructure;
  const WriteAbaqusHexahedronInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
