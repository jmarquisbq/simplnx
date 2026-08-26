#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/Common/Result.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/Arguments.hpp"
#include "simplnx/Filter/IFilter.hpp"

#include <array>
#include <filesystem>

namespace fs = std::filesystem;

namespace nx::core
{
/**
 * @class ReadStlFile
 * @brief Reads a binary STL mesh into a TriangleGeom.
 *
 * Header markers select compatibility behavior for Magics color attributes and
 * VXelements metadata. Final node elimination merges shared vertices and applies scaling.
 */
class SIMPLNXCORE_EXPORT ReadStlFile
{
public:
  /**
   * @brief Creates a binary STL reader.
   * @param dataStructure Receives mesh arrays.
   * @param stlFilePath Identifies the input file.
   * @param geometryPath Identifies the destination TriangleGeom.
   * @param faceGroupPath Is retained but not used.
   * @param faceNormalsDataPath Identifies the destination normal array.
   * @param scaleOutput Applies scaleFactor during duplicate-node elimination when true.
   * @param scaleFactor Specifies output coordinate scale.
   * @param shouldCancel Stops before later triangles when true.
   * @param mesgHandler Receives progress messages.
   */
  ReadStlFile(DataStructure& dataStructure, fs::path stlFilePath, const DataPath& geometryPath, const DataPath& faceGroupPath, const DataPath& faceNormalsDataPath, bool scaleOutput,
              float32 scaleFactor, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler);
  /**
   * @brief Destroys the non-owning reader.
   */
  ~ReadStlFile() noexcept;

  ReadStlFile(const ReadStlFile&) = delete;
  ReadStlFile(ReadStlFile&&) noexcept = delete;
  ReadStlFile& operator=(const ReadStlFile&) = delete;
  ReadStlFile& operator=(ReadStlFile&&) noexcept = delete;

  /**
   * @brief Reads triangles and eliminates duplicate nodes.
   * @return File, parse, or node-elimination error, or success after cancellation.
   *
   * Cancellation and parse errors can retain partially written mesh arrays.
   * Per-value DataStore writes do not report I/O errors.
   */
  Result<> operator()();

  /**
   * @brief Reads the configured STL file.
   * @return Parse or geometry error, or success.
   * @warning This declaration has no definition in the current library.
   */
  Result<> readFile();

private:
  DataStructure& m_DataStructure;
  const fs::path m_FilePath;
  const DataPath& m_GeometryDataPath;
  const DataPath& m_FaceGroupPath;
  const DataPath m_FaceNormalsDataPath;
  const bool m_ScaleOutput = false;
  const float m_ScaleFactor = 1.0F;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
