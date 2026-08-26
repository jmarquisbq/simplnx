#include "ReadStlFile.hpp"

#include "SimplnxCore/utils/StlUtilities.hpp"

#include "simplnx/Common/Range.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/Geometry/TriangleGeom.hpp"
#include "simplnx/Utilities/DataArrayUtilities.hpp"
#include "simplnx/Utilities/GeometryUtilities.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <cstdio>
#include <utility>

using namespace nx::core;

namespace
{
/**
 * @class StlFileSentinel
 * @brief Closes one owned FILE handle at scope exit.
 */
class StlFileSentinel
{
public:
  /**
   * @brief Takes ownership of an open file handle.
   * @param file Specifies a non-null handle.
   */
  explicit StlFileSentinel(FILE* file)
  : m_File(file)
  {
  }
  /**
   * @brief Closes the owned handle and ignores close status.
   */
  ~StlFileSentinel()
  {
    std::ignore = std::fclose(m_File);
  }
  StlFileSentinel(const StlFileSentinel&) = delete;
  StlFileSentinel(StlFileSentinel&&) = delete;
  StlFileSentinel& operator=(const StlFileSentinel&) = delete;
  StlFileSentinel& operator=(StlFileSentinel&&) = delete;

private:
  FILE* m_File = nullptr;
};

/**
 * @brief Detects Magics color-attribute header markers.
 * @param stlHeaderStr Provides the 80-byte STL header.
 * @return True when both color and material markers exist.
 *
 * Magics stores color data in the attribute field instead of a metadata length.
 */
bool IsMagicsFile(const std::string& stlHeaderStr)
{
  static const std::string k_ColorHeader("COLOR=");
  static const std::string k_MaterialHeader("MATERIAL=");
  if(stlHeaderStr.find(k_ColorHeader) != std::string::npos && stlHeaderStr.find(k_MaterialHeader) != std::string::npos)
  {
    return true;
  }
  return false;
}

/**
 * @brief Detects the VXelements vendor header marker.
 * @param stlHeader Provides the 80-byte STL header.
 * @return True when the VXelements marker exists.
 *
 * VXelements does not use the triangle attribute field as a metadata length.
 */
bool IsVxElementsFile(const std::string& stlHeader)
{
  return nx::core::StringUtilities::contains(stlHeader, "VXelements");
}
} // End anonymous namespace

ReadStlFile::ReadStlFile(DataStructure& dataStructure, fs::path stlFilePath, const DataPath& geometryPath, const DataPath& faceGroupPath, const DataPath& faceNormalsDataPath, bool scaleOutput,
                         float32 scaleFactor, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler)
: m_DataStructure(dataStructure)
, m_FilePath(std::move(stlFilePath))
, m_GeometryDataPath(geometryPath)
, m_FaceGroupPath(faceGroupPath)
, m_FaceNormalsDataPath(faceNormalsDataPath)
, m_ScaleOutput(scaleOutput)
, m_ScaleFactor(scaleFactor)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

ReadStlFile::~ReadStlFile() noexcept = default;

Result<> ReadStlFile::operator()()
{
  std::error_code errorCode;
  auto stlFileSize = std::filesystem::file_size(m_FilePath, errorCode);

  FILE* f = std::fopen(m_FilePath.string().c_str(), "rb");
  if(nullptr == f)
  {
    return MakeErrorResult(nx::core::StlConstants::k_ErrorOpeningFile, "Error opening STL file");
  }
  StlFileSentinel fileSentinel(f);

  std::array<char, nx::core::StlConstants::k_STL_HEADER_LENGTH> stlHeader = {0};
  int32_t triCount = 0;
  if(std::fread(stlHeader.data(), nx::core::StlConstants::k_STL_HEADER_LENGTH, 1, f) != 1)
  {
    return MakeErrorResult(nx::core::StlConstants::k_StlHeaderParseError, "Error reading first 8 bytes of STL header. This can't be good.");
  }

  std::string stlHeaderStr(stlHeader.data(), nx::core::StlConstants::k_STL_HEADER_LENGTH);

  bool ignoreMetaSizeValue = (IsMagicsFile(stlHeaderStr) || IsVxElementsFile(stlHeaderStr) ? true : false);

  if(std::fread(&triCount, sizeof(int32_t), 1, f) != 1)
  {
    return MakeErrorResult(nx::core::StlConstants::k_TriangleCountParseError, "Error reading number of triangles from file. This is bad.");
  }

  auto& triangleGeom = m_DataStructure.getDataRefAs<TriangleGeom>(m_GeometryDataPath);

  triangleGeom.resizeFaceList(triCount);
  triangleGeom.resizeVertexList(triCount * 3);

  using SharedTriList = AbstractDataStore<IGeometry::MeshIndexArrayType::value_type>;
  using SharedVertList = AbstractDataStore<IGeometry::SharedVertexList::value_type>;

  SharedTriList& triangles = triangleGeom.getFaces()->getDataStoreRef();
  SharedVertList& nodes = triangleGeom.getVertices()->getDataStoreRef();

  auto& faceNormalsStore = m_DataStructure.getDataAs<Float64Array>(m_FaceNormalsDataPath)->getDataStoreRef();

  constexpr size_t k_StlElementCount = 12;
  std::array<float, k_StlElementCount> fileVert = {0.0F};
  uint16_t attr = 0;
  std::vector<uint8_t> triangleAttributeBuffer(std::numeric_limits<uint16_t>::max());

  MessageHelper messageHelper(m_MessageHandler);
  ThrottledMessenger throttledMessenger = messageHelper.createThrottledMessenger();

  fpos_t pos;

  // Check progress every 10,000 triangles to reduce work in the read loop.
  constexpr int32_t k_ProgressStride = 10000;
  for(int32_t t = 0; t < triCount; ++t)
  {
    if(t % k_ProgressStride == 0)
    {
      throttledMessenger.sendThrottledMessage([&]() { return fmt::format("Reading {:.2f}% Complete", CalculatePercentComplete(t, triCount)); });
    }
    if(m_ShouldCancel)
    {
      return {};
    }
    fgetpos(f, &pos);
#if defined(__APPLE__) || defined(_WIN32)
    if(pos >= stlFileSize)
#else
    if(pos.__pos >= stlFileSize)
#endif
    {
      std::string msg = fmt::format(
          "Trying to read at file position {} >= file size {}.\n  File Header: '{}'\n  Header Triangle Count: {}  Current Triangle: {}\n  The STL File does not conform to the STL file specification.",
#if defined(__APPLE__) || defined(_WIN32)
          pos,
#else
          pos.__pos,
#endif
          stlFileSize, stlHeaderStr, triCount, t);
      return MakeErrorResult(nx::core::StlConstants::k_StlFileLengthError, msg);
    }

    size_t objsRead = std::fread(fileVert.data(), sizeof(float), k_StlElementCount, f);
    if(k_StlElementCount != objsRead)
    {
      std::string msg = fmt::format("Error reading Triangle '{}'. Object Count was {} and should have been {}", t, objsRead, k_StlElementCount);
      return MakeErrorResult(nx::core::StlConstants::k_TriangleParseError, msg);
    }
    // Standard files use this field as a vendor-metadata byte count.
    objsRead = std::fread(&attr, sizeof(uint16_t), 1, f);
    if(objsRead != 1)
    {
      std::string msg = fmt::format("Error reading Number of attributes for triangle '{}'. uint16 count was {} and should have been 1", t, objsRead);
      return MakeErrorResult(nx::core::StlConstants::k_AttributeParseError, msg);
    }
    // Honor the metadata length unless a detected vendor uses the field for other data.
    if(attr > 0 && !ignoreMetaSizeValue)
    {
      std::ignore = std::fseek(f, static_cast<size_t>(attr), SEEK_CUR);
    }

    faceNormalsStore[3 * t + 0] = static_cast<double>(fileVert[0]);
    faceNormalsStore[3 * t + 1] = static_cast<double>(fileVert[1]);
    faceNormalsStore[3 * t + 2] = static_cast<double>(fileVert[2]);
    nodes[3 * (3 * t + 0) + 0] = fileVert[3];
    nodes[3 * (3 * t + 0) + 1] = fileVert[4];
    nodes[3 * (3 * t + 0) + 2] = fileVert[5];
    nodes[3 * (3 * t + 1) + 0] = fileVert[6];
    nodes[3 * (3 * t + 1) + 1] = fileVert[7];
    nodes[3 * (3 * t + 1) + 2] = fileVert[8];
    nodes[3 * (3 * t + 2) + 0] = fileVert[9];
    nodes[3 * (3 * t + 2) + 1] = fileVert[10];
    nodes[3 * (3 * t + 2) + 2] = fileVert[11];
    triangles[t * 3] = 3 * t + 0;
    triangles[t * 3 + 1] = 3 * t + 1;
    triangles[t * 3 + 2] = 3 * t + 2;
  }

  return GeometryUtilities::EliminateDuplicateNodes(triangleGeom, m_ScaleOutput ? std::optional<float32>(m_ScaleFactor) : std::nullopt);
}
