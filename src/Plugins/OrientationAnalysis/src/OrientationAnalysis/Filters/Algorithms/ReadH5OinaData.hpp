#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"
#include "OrientationAnalysis/utilities/IEbsdOemReader.hpp"

namespace nx::core
{

/**
 * @class ReadH5OinaData
 * @brief Imports Oxford OINA HDF5 EBSD scans into an ImageGeom.
 *
 * EbsdLib owns one scan at a time. This importer copies channels, converts phase
 * storage, and applies optional hexagonal Euler correction in pages of at most
 * 65,536 tuples. The bounded transfers support out-of-core destinations.
 *
 * Cancellation returns success and preserves completed destination pages.
 */

class ORIENTATIONANALYSIS_EXPORT ReadH5OinaData : public IEbsdOemReader<ebsdlib::H5OINAReader>
{
public:
  /**
   * @brief Initializes an OINA scan importer.
   * @param dataStructure Provides destination arrays and geometry.
   * @param mesgHandler Receives status messages.
   * @param shouldCancel Signals cancellation between destination pages.
   * @param inputValues Identifies scans, options, and destination paths.
   * @pre All arguments outlive this importer.
   */
  ReadH5OinaData(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ReadH5DataInputValues* inputValues);
  ~ReadH5OinaData() noexcept override;

  ReadH5OinaData(const ReadH5OinaData&) = delete;
  ReadH5OinaData(ReadH5OinaData&&) noexcept = delete;
  ReadH5OinaData& operator=(const ReadH5OinaData&) = delete;
  ReadH5OinaData& operator=(ReadH5OinaData&&) noexcept = delete;

  /**
   * @brief Imports each selected scan through IEbsdOemReader.
   * @return Reader, destination transfer, or missing-pattern-data errors.
   */
  Result<> operator()();

  /**
   * @brief Converts and copies one loaded scan into its volume range.
   * @param index Zero-based scan index.
   * @return Source, destination transfer, or missing-pattern-data errors.
   */
  Result<> copyRawEbsdData(int index) override;
};

} // namespace nx::core
