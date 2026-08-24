#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/FileSystemPathParameter.hpp"

#include <EbsdLib/IO/HKL/CtfReader.h>

namespace nx::core
{

struct ORIENTATIONANALYSIS_EXPORT ReadCtfDataInputValues
{
  FileSystemPathParameter::ValueType InputFile;
  DataPath DataContainerName;
  std::string CellAttributeMatrixName;
  std::string CellEnsembleAttributeMatrixName;
  bool DegreesToRadians;
  bool EdaxHexagonalAlignment;
};

/**
 * @class ReadCtfData
 * @brief Algorithm that reads a single .ctf (Oxford/HKL) EBSD file into an Image Geometry.
 *
 * Parses the .ctf file using EbsdLib's CtfReader, then transfers the parsed data
 * into the DataStructure's cell-level and ensemble-level arrays.
 *
 * @section ooc_summary OOC Optimization Summary
 * All data transfer uses copyFromBuffer() bulk writes. Euler angles are interleaved from
 * 3 source arrays using chunked buffers with optional hex correction and degree-to-radian
 * conversion. Crystal structures are cached locally for the per-cell hex correction check.
 */
class ORIENTATIONANALYSIS_EXPORT ReadCtfData
{
public:
  ReadCtfData(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, ReadCtfDataInputValues* inputValues);
  ~ReadCtfData() noexcept;

  ReadCtfData(const ReadCtfData&) = delete;
  ReadCtfData(ReadCtfData&&) noexcept = delete;
  ReadCtfData& operator=(const ReadCtfData&) = delete;
  ReadCtfData& operator=(ReadCtfData&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ReadCtfDataInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;

  /**
   * @brief Populates the Ensemble Attribute Matrix arrays (CrystalStructures, MaterialName,
   * LatticeConstants) from the phase sections that CtfReader parsed out of the file header.
   * @param reader The CtfReader instance that has already successfully read the file.
   * @return Invalid Result if the file declares no phases.
   */
  Result<> loadMaterialInfo(ebsdlib::CtfReader* reader) const;

  /**
   * @brief Copies the per-scan-point data columns out of the CtfReader buffers into the
   * Cell Attribute Matrix arrays, condensing Euler1/2/3 into the 3-component EulerAngles
   * array and applying the optional EDAX hexagonal alignment (+30 degrees) and
   * degrees-to-radians conversions.
   * @param reader The CtfReader instance that has already successfully read the file.
   * @return Invalid Result if the reader buffers cannot back the geometry created at preflight.
   */
  Result<> copyRawEbsdData(ebsdlib::CtfReader* reader) const;
};

} // namespace nx::core
