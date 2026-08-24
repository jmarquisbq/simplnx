#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/FileSystemPathParameter.hpp"

#include <EbsdLib/IO/TSL/AngReader.h>

namespace nx::core
{

/**
 * @brief Input values for the ReadAngData algorithm.
 */
struct ORIENTATIONANALYSIS_EXPORT ReadAngDataInputValues
{
  FileSystemPathParameter::ValueType InputFile; ///< Path to the .ang EBSD data file.
  DataPath DataContainerName;                   ///< Path to the output DataContainer (ImageGeom).
  std::string CellAttributeMatrixName;          ///< Name of the cell-level AttributeMatrix.
  std::string CellEnsembleAttributeMatrixName;  ///< Name of the ensemble-level AttributeMatrix.
};

/**
 * @class ReadAngData
 * @brief Algorithm that reads a single .ang EBSD file into an Image Geometry.
 *
 * Parses the .ang file using EbsdLib's AngReader, then transfers the parsed data
 * into the DataStructure's cell-level and ensemble-level arrays.
 *
 * @section ooc_summary OOC Optimization Summary
 * All data transfer from the EbsdLib reader buffers into the DataStructure uses
 * copyFromBuffer() bulk writes instead of per-element operator[] access. Euler angles
 * (3 separate source arrays interleaved into 1 destination) use a chunked buffer approach
 * to bound memory while maintaining bulk I/O efficiency. See copyRawEbsdData() for details.
 */
class ORIENTATIONANALYSIS_EXPORT ReadAngData
{
public:
  ReadAngData(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, ReadAngDataInputValues* inputValues);
  ~ReadAngData() noexcept;

  ReadAngData(const ReadAngData&) = delete;
  ReadAngData(ReadAngData&&) = delete;
  ReadAngData& operator=(const ReadAngData&) = delete;
  ReadAngData& operator=(ReadAngData&&) = delete;

  /**
   * @brief Executes the algorithm: reads the .ang file and populates the DataStructure.
   * @return Result<> indicating success or an EbsdLib error.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const IFilter::MessageHandler& m_MessageHandler;
  const std::atomic_bool& m_ShouldCancel;
  const ReadAngDataInputValues* m_InputValues = nullptr;

  /**
   * @brief Populates the Ensemble Attribute Matrix arrays (CrystalStructures, MaterialName,
   * LatticeConstants) from the phase sections parsed out of the .ang header. Every slot is
   * first initialized to the "Invalid Phase" defaults, then overwritten per parsed phase.
   * @param reader The AngReader that has already successfully read the input file.
   * @return Error result if no phases were parsed or a phase index falls outside the ensemble arrays.
   */
  Result<> loadMaterialInfo(ebsdlib::AngReader* reader) const;

  /**
   * @brief Copies the per-point data columns from the AngReader into the Cell Attribute Matrix
   * arrays: remaps phase values < 1 to 1, interleaves phi1/PHI/phi2 into the 3-component
   * EulerAngles array, and copies the remaining columns verbatim.
   * @param reader The AngReader that has already successfully read the input file.
   * @return Error result if the reader produced fewer scan points than the preflight-sized geometry
   * expects (which would otherwise read past the reader's buffers).
   */
  Result<> copyRawEbsdData(ebsdlib::AngReader* reader) const;
};

} // namespace nx::core
