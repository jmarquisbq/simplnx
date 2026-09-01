# Iso Contour Distance Image Filter

Compute a narrow-band signed distance to a level-set iso-contour of the input image.

## Group (Subgroup)

ImageProcessing (Morphology)

## Description

Produces a **float32** image in which the value at each voxel adjacent to the **Level Set Value** iso-contour is the sub-pixel signed distance to that contour, computed by gradient interpolation. Voxels that are not adjacent to a crossing are set to **+Far Value** (where the input is above the level set) or **-Far Value** (below it), and voxels exactly on the level set are 0.

This is the classic initializer for level-set and fast-marching methods: it produces an accurate distance only in a one-voxel band around the contour, leaving the rest of the image at the saturated far value. It is a single neighborhood pass (not a full distance transform); when a full signed distance is required, prefer the Signed Maurer or Signed Danielsson Distance Map Image Filters.

The input array must be single-component (scalar) and may be **any numeric type, integer or floating point**. The output is a fixed **float32** image regardless of the input element type. This is an ITK-free, out-of-core-capable reimplementation of the legacy ITK Iso Contour Distance Image Filter, and matches it exactly.

### Level Set Value

The value of the level set (iso-contour) whose distance is computed. Default is 0.

### Far Value

The signed value written to voxels away from the iso-contour: +Far Value where the input is above the level set, -Far Value where it is below. Default is 10.

## Algorithm

The filter examines each voxel and its axis neighbors, detects level-set crossings, and keeps the smallest gradient-interpolated signed distance. Voxels away from a crossing retain the selected positive or negative **Far Value**.

### In-Core Path

Resident arrays use a parallel gather. Each worker reads from the complete input and writes a separate output voxel, so no worker writes through a shared `DataStore` interface.

### Out-of-Core Path

For true 3-D disk-backed images, the filter first requests enough shared working memory for one complete input buffer and one float32 output buffer. The request scales with dimensions and input type; a `512 x 512 x 128` uint8 image uses 160 MiB, while a float64 input uses 384 MiB. The fast gather runs only after a complete grant, using one bulk read and one bulk write.

If the complete request is unavailable or allocation fails, the existing 3-D rolling-plane engine remains the bounded fallback. True 2-D always uses bounded row blocks or X tiles. All requests share the application cache budget and remain subject to its aggregate 25% limit.

% Auto generated parameter table will be inserted here

## Example Pipelines

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
