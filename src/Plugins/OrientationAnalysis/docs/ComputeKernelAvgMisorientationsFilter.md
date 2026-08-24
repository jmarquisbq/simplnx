# Compute Kernel Average Misorientations

## Group (Subgroup)

Statistics (Crystallography)

## Description

This **Filter** determines the Kernel Average Misorientation (KAM) for each **Cell**. This **Filter** requires an **Image Geometry**, and the output KAM values are stored in degrees.  The user can select the size of the kernel to be used in the calculation.  The kernel size entered by the user is the *radius* of the kernel (i.e., entering values of *1*, *2*, *3* will result in a kernel that is *3*, *5*, and *7* **Cells** in size in the X, Y and Z directions, respectively).  The algorithm for determination of KAM is as follows:

1. Calculate the misorientation angle between each **Cell** in a kernel and the central **Cell** of the kernel
2. Average all of the misorientations for the kernel and store at the central **Cell**

The **Use Feature Ids** option controls which **Cells** within the kernel are included in the average:

+ **Checked (default):** only **Cells** that belong to the same *Feature* (same *Feature Id*) as the central **Cell** are considered — the calculation will **not** cross grain boundaries. This is the traditional per-grain KAM.
+ **Unchecked:** the *Feature Id* grouping is ignored and the average may cross grain boundaries, producing a per-voxel KAM. A kernel **Cell** is still excluded if its *Feature Id* is 0 (invalid/background data) or if its *Phase* differs from the central **Cell**'s *Phase* (averaging is restricted to Cells of the same Phase).

In both modes, **Cells** with a *Feature Id* of 0 or a *Phase* of 0 are considered invalid and receive a KAM value of 0.

*Note:* All **Cells** in the kernel are weighted equally during the averaging, though they are not equidistant from the central **Cell**.

For related per-feature misorientation metrics, see the **Compute Feature Reference Misorientations** and **Compute Misorientation** filters.

## Algorithm

For each valid cell in the Image Geometry, the algorithm examines all cells within the user-specified kernel radius in X, Y, and Z. In per-grain mode, only neighbors with the same feature ID as the center cell are included. In per-voxel mode, any neighbor with a positive feature ID and the same phase as the center cell is included. The crystallographic misorientation angle between the center cell's quaternion and each qualifying neighbor's quaternion is computed using the appropriate LaueOps symmetry operators. The average of these misorientation angles is stored as the KAM value for the center cell.

### In-Core Path

For in-memory arrays, the filter uses its established parallel direct traversal.

### Out-of-Core Path

For out-of-core arrays, the filter processes one output Z plane at a time. It keeps a rolling, cache-budgeted window of **Feature IDs**, **Cell Phases**, and quaternions, and bulk-writes one output plane. Parallel workers access only these local buffers and write disjoint output ranges.

### Memory Use

The filter derives a cap for its dominant array-buffer payload from the **CacheMemoryBudgetManager**. If the clamped rolling window does not fit that cap, it uses a fixed tuple-block LRU cache whose input-cache and output buffers fit the cap instead of allocating an unrestricted Z slab. Fixed executor overhead, such as cache metadata and the small ensemble table, is independent of volume depth.

% Auto generated parameter table will be inserted here

## Example Pipelines

+ (04) Small IN100 Crystallographic Statistics
+ EBSD_File_Processing/aptr12_Analysis
+ EBSD_File_Processing/avtr12_Analysis

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
