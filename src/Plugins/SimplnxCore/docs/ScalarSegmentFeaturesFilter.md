# Segment Features (Scalar)

## Group (Subgroup)

Reconstruction (Segmentation)

## Description

This **Filter** groups neighboring **Cells** (voxels) that have similar scalar values into **Features** (connected regions). The output is a *FeatureIds* array that assigns every cell in the input **Image Geometry** to a numbered feature, so that downstream analyses can operate on features instead of individual cells.

Use this filter when the segmentation criterion is a single integer value per cell, such as a class label or an integer grayscale intensity. The current filter parameters accept integer input arrays and an integer tolerance; floating-point quality arrays require a documented conversion or a different segmentation method. For orientation-based segmentation of EBSD data, see [Segment Features (Misorientation)](../OrientationAnalysis/EBSDSegmentFeaturesFilter.md) or [Segment Features (C-Axis Misalignment)](../OrientationAnalysis/CAxisSegmentFeaturesFilter.md).

### What is Feature Segmentation?

Segmentation groups cells that "belong together" into discrete regions. Each region receives a unique positive integer *Feature Id*; cells that are excluded (for example, by a mask) are assigned Feature Id 0. After segmentation, a new **Feature Attribute Matrix** is created so that per-feature statistics (sizes, centroids, phases, etc.) can be computed and stored.

![Fig. 1: DREAM3D-NX groups cells into features (grains) via the FeatureIds array, and features into ensembles (phases). Per-feature data lives in the Cell Feature Data attribute matrix; per-phase data lives in Cell Ensemble Data.](Images/DataModel_CellFeatureEnsemble.png)

### How This Filter Works

The filter labels connected components of eligible cells. Two adjacent cells connect when their scalar values differ by **at most** the *Scalar Tolerance*. Connectivity propagates through a chain of qualifying neighboring pairs; values at opposite ends of one feature can therefore differ by more than the tolerance. The comparison is not a limit on the full feature's range or its difference from one seed.

With tolerance *0*, neighboring integer values must be equal. Without a mask, zero-valued input cells are also eligible and receive positive feature IDs. A scalar value of zero is not automatically background. Enable a mask when a class should be excluded before segmentation.

### Tolerance and Units

The *Scalar Tolerance* is a nonnegative **integer in the input scalar's units**. For a 0/1 class image, use *0* to keep different classes separate. A tolerance of *1* would allow the two classes to connect. For an 8-bit grayscale image, a tolerance of *5* permits adjacent intensities that differ by at most five intensity levels; it does not define a calibrated material distinction.

A smaller tolerance usually produces more components. Inspect the segmentation against the intended task before increasing it. Changing integer scaling or quantization also changes the meaning of a numeric tolerance.

## Algorithm

The implementation uses slice-buffered connected-component labeling. It compares neighboring scalar values after conversion to `float64`, then resizes the feature table and marks positive feature rows active. Feature row zero remains reserved. Optional ID randomization changes the labels used for display; it does not define new pixel memberships. Very large 64-bit integer values can lose exact distinctions when converted to `float64`; the small 0/1 integers in this example are represented exactly.

### Neighbor Scheme

The *Neighbor Scheme* parameter provides the following choices:

- **Face Neighbors [0]**: Only the 6 face-sharing neighbors of a voxel are considered during segmentation.
- **All Connected Neighbors [1]**: All 26 neighbors connected by a face, edge, or vertex are considered during segmentation.

DREAM.3D version 6.x only used face neighbors. The default here is still *Face Only* for backward compatibility; switch to *All Connected* when diagonal connectivity should merge a feature that would otherwise be split.

| Neighbor Scheme = "Face Only" | Neighbor Scheme = "All Connected" |
|:--:|:--:|
| ![Shared Edges - Neighbor Scheme = "Face Only"](Images/SegmentFeatures/shared_edges_face_only.png) | ![Shared Edges - Neighbor Scheme = "All Connected"](Images/SegmentFeatures/shared_edges_all_connected.png) |

| Neighbor Scheme = "Face Only" | Neighbor Scheme = "All Connected" |
|:--:|:--:|
| ![Shared Points - Neighbor Scheme = "Face Only"](Images/SegmentFeatures/shared_points_face_only.png) | ![Shared Points - Neighbor Scheme = "All Connected"](Images/SegmentFeatures/shared_points_all_connected.png) |

| Neighbor Scheme = "Face Only" | Neighbor Scheme = "All Connected" |
|:--:|:--:|
| ![Disconnected Regions - Neighbor Scheme = "Face Only"](Images/SegmentFeatures/nothing_shared_face_only.png) | ![Disconnected Regions - Neighbor Scheme = "All Connected"](Images/SegmentFeatures/nothing_shared_all_connected.png) |

| Neighbor Scheme = "Face Only" | Neighbor Scheme = "All Connected" |
|:--:|:--:|
| ![Shared Edges & Points With Disconnected Region - "Face Only"](Images/SegmentFeatures/combination_face_only.png) | ![Shared Edges & Points With Disconnected Region - "All Connected"](Images/SegmentFeatures/combination_all_connected.png) |

### Mask Array

If *Use Mask Array* is enabled, cells flagged *false* in the mask are excluded from segmentation and left with a Feature Id of 0. Masks are commonly used to restrict segmentation to valid sample regions -- for example, a quality threshold on an EBSD confidence index (see [Multi-Threshold Objects](MultiThresholdObjectsFilter.md)).

### Periodic Option

If the input data represents a **periodic** structure (i.e., features are allowed to wrap across opposite faces of the volume), enable *Is Periodic*. The filter will detect features that tile across the geometry bounds and emit a warning that centroid and other spatial statistics may be incorrect for those features.

### Required Input Sources

- **Input Scalar Array** -- a single-component cell-level integer array. Common sources include image readers such as [Read Image Stack](../ImageProcessing/ReadImageStackFilter.md), phase or quality arrays from EBSD readers such as [Read H5EBSD](../OrientationAnalysis/ReadH5EbsdFilter.md), or any computed per-cell scalar from an earlier filter in the pipeline.
- **Mask Array** (optional) -- a boolean array marking valid cells, typically produced by [Multi-Threshold Objects](MultiThresholdObjectsFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

+ (09) Image Segmentation

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Ti64_Segmentation_Examples/(03) Ti64 Minimum Size Cleanup.d3dpipeline` (ImageProcessing)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
