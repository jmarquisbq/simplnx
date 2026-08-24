#include "simplnx/Utilities/Parsing/HDF5/DeflateEligibility.hpp"

#include "simplnx/Utilities/Parsing/HDF5/H5Support.hpp"

#include <H5Dpublic.h>
#include <H5Ppublic.h>
#include <H5Tpublic.h>
#include <H5Zpublic.h>

#include <cstdint>
#include <mutex>

namespace nx::core::HDF5
{

bool hostIsLittleEndian()
{
  const uint16_t probe = 0x0001;
  return *reinterpret_cast<const uint8_t*>(&probe) == 0x01;
}

bool probeSingleDeflateEligibility(hid_t datasetId, usize elementSize, int32* deflateLevelOut)
{
  std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());

  // Single-filter deflate pipeline? Also capture the deflate level (cd_values[0]) when the
  // caller asked for it, so a writer's compress2 produces exactly what the dataset's
  // creation property list specifies.
  hid_t dcpl = H5Dget_create_plist(datasetId);
  if(dcpl < 0)
  {
    return false;
  }
  const int nFilters = H5Pget_nfilters(dcpl);
  bool deflateOnly = false;
  if(nFilters == 1)
  {
    unsigned int flags = 0;
    size_t cdNelmts = 16; // in: capacity of cdValues; out: number of client values
    unsigned int cdValues[16] = {0};
    char name[64] = {0};
    unsigned int filterConfig = 0;
    const H5Z_filter_t filterId = H5Pget_filter2(dcpl, 0, &flags, &cdNelmts, cdValues, sizeof(name), name, &filterConfig);
    deflateOnly = (filterId == H5Z_FILTER_DEFLATE);
    if(deflateOnly && cdNelmts >= 1 && deflateLevelOut != nullptr)
    {
      *deflateLevelOut = static_cast<int32>(cdValues[0]);
    }
  }
  H5Pclose(dcpl);
  if(!deflateOnly)
  {
    return false;
  }

  // Byte-order gate: the fast paths bypass the file<->host swap H5Dread/H5Dwrite would
  // perform, so eligible only when the file element order matches the host (or the
  // element is single-byte, order-irrelevant).
  if(elementSize == 1)
  {
    return true;
  }
  hid_t dtype = H5Dget_type(datasetId);
  const H5T_order_t order = H5Tget_order(dtype);
  H5Tclose(dtype);
  const H5T_order_t hostOrder = hostIsLittleEndian() ? H5T_ORDER_LE : H5T_ORDER_BE;
  return order == hostOrder;
}

} // namespace nx::core::HDF5
