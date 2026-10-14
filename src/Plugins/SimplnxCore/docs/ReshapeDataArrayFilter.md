# Reshape Data Array

## Group (Subgroup)

Core (Generation)

## Description

This **Filter** replaces an array with a copy using new **tuple dimensions**, preserving its component shape and flat tuple order. Use the same total tuple count for a lossless shape change. The implementation also permits a different count: a smaller result retains only the leading tuples, and a larger result retains the original tuples followed by newly allocated entries. Do not use count-changing behavior when the intention is only to reinterpret layout.

## ⚠ CRITICAL WARNING: Values Are Not Reordered

**This filter does not transpose or reorder values.** It allocates a replacement array and copies the common leading tuple range. If existing flat order does not match the intended new layout, downstream filters can misinterpret the result even when total tuple counts agree.

### When This Is Safe vs Dangerous

DREAM3DNX uses **C-order (row-major)** layout: the last dimension varies fastest in memory. So `(3, 45)` is stored as 3 rows × 45 columns -- 3 contiguous chunks of 45 values each.

- **Safe**: changing `(135)` → `(3, 45)` simply tells the system "treat this 135-tuple array as a 3×45 array". The bytes in memory are the same; only the shape interpretation changes.
- **Safe**: changing `(3, 45)` → `(135)` flattens to 1D. Memory is identical.
- **DANGEROUS**: reading an array from an HDF5 file that was stored 3 rows × 45 columns and trying to use it as 3-component 45-vertex data. In DREAM3DNX, 3-vertex data is stored as `(N, 3)` with 3 contiguous values per tuple. Calling Reshape from `(3, 45)` to `(45, 3)` **does not transpose** -- it just relabels the shape, and the bytes are now in the wrong order for the new shape.

To actually transpose or reorder data, use external preprocessing (e.g., reshape the HDF5 dataset before reading) or use an array-arithmetic filter to manually rebuild the array with the desired ordering.

### Validation

Use positive integer dimensions and independently check that their product equals the original tuple count when preserving all values. A changed total count is allowed by the implementation, so successful preflight does not establish a lossless reshape. An unchanged shape is rejected as unnecessary. Reshape a free array outside an Attribute Matrix when its new tuple shape would disagree with that matrix's other arrays.

### Neighbor Lists and String Arrays

Neighbor Lists and String Arrays do not support multi-dimensional tuple shapes. If you specify a multi-dimensional new shape for one of these array types, the filter emits a warning and falls back to an equivalent 1-D tuple count.

### Required Input Sources

- **Array to Reshape** -- any **Data Array**, **Neighbor List**, or **String Array** in the Data Structure.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `T12_Import_Examples/(01) T12 Typed CSV Import.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
