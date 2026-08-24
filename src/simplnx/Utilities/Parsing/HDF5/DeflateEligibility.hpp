#pragma once

#include "simplnx/Common/Types.hpp"
#include "simplnx/simplnx_export.hpp"

#include <H5Ipublic.h>

namespace nx::core::HDF5
{

/**
 * @brief Runtime host-endianness probe (portable across MSVC/GCC/Clang; folds to a constant).
 * @return true when the host stores multi-byte integers little-endian first.
 */
SIMPLNX_EXPORT bool hostIsLittleEndian();

/**
 * @brief Probes whether a dataset qualifies for the HDF5-bypassing deflate fast paths —
 * reading raw compressed chunks via a parallel pread + inflate, or writing them via a
 * parallel compress + H5Dwrite_chunk — instead of the serial HDF5 filter pipeline.
 *
 * A dataset is eligible when BOTH hold:
 * - its filter pipeline is exactly one filter and that filter is deflate — any other
 *   pipeline (shuffle/szip/fletcher32/multi-filter/unknown) must go through HDF5's own
 *   serial filter pipeline for correctness; and
 * - the file element byte order matches the host order, or the element is single-byte
 *   (order-irrelevant) — both fast paths bypass the byte swap H5Dread/H5Dwrite would
 *   perform.
 *
 * The loader and writer share this single probe so their read and write gates cannot
 * diverge: a chunk written by the writer's fast path is always readable by the loader's,
 * and vice versa.
 *
 * Acquires @c nx::core::HDF5::Support::ApiLock() internally for the DCPL/datatype
 * queries; the caller must NOT already hold it. That lock is a non-recursive
 * std::mutex, so re-entering it on the same thread would self-deadlock.
 *
 * @param datasetId Open HDF5 dataset id.
 * @param elementSize sizeof(T) for the dataset element type (gates the byte-order check).
 * @param deflateLevelOut Optional out-parameter; when non-null and the dataset is a
 *        single-filter deflate pipeline, receives the DCPL deflate level (cd_values[0])
 *        so a writer compresses exactly as the dataset's creation property list
 *        specifies. Untouched when the pipeline is not single-filter deflate.
 * @return true when the parallel deflate fast paths apply.
 */
SIMPLNX_EXPORT bool probeSingleDeflateEligibility(hid_t datasetId, usize elementSize, int32* deflateLevelOut);

} // namespace nx::core::HDF5
