#pragma once

#include "simplnx/simplnx_export.hpp"

#include "simplnx/Utilities/ImageIO/IImageIO.hpp"

namespace nx::core
{

/**
 * @class StbImageIO
 * @brief IImageIO backend using stb_image (read) and stb_image_write (write)
 *        for PNG, JPEG, and BMP formats.
 */
class SIMPLNX_EXPORT StbImageIO : public IImageIO
{
public:
  StbImageIO() = default;
  ~StbImageIO() noexcept override = default;

  Result<ImageMetadata> readMetadata(const std::filesystem::path& filePath) const override;
  Result<> readPixelData(const std::filesystem::path& filePath, std::span<uint8> buffer) const override;

  /**
   * @brief Decodes with stb's required whole-image allocation, then exposes
   * row spans directly without creating a second full-image staging buffer.
   */
  Result<> readPixelDataRows(const std::filesystem::path& filePath, const ReadRowCallback& callback) const override;
  Result<> writePixelData(const std::filesystem::path& filePath, std::span<const uint8> buffer, const ImageMetadata& metadata) const override;
  std::set<DataType> supportedWriteDataTypes() const override;
  std::set<usize> supportedWriteComponentCounts() const override;
};

} // namespace nx::core
