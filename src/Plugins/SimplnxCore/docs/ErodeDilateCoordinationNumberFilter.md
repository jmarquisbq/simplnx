# Erode/Dilate Coordination Number

## Group (Subgroup)

Processing (Cleanup)

## Description

This **Filter** changes interfaces between cells with **Feature Id 0** and cells with a **positive Feature Id**. It can fill missing cells or remove isolated positive cells. It does not treat two different positive IDs as opposite classes, so it is not a general grain-boundary smoothing filter.

The *coordination number* counts face-sharing neighbors with opposite zero-versus-positive status. Only neighbors inside the **Image Geometry** count: the maximum is **6 in 3D**, **4 in a single XY slice**, and fewer at its edges.

| Center and neighbor | Adds to coordination number? |
| --- | --- |
| 0 and a positive ID | Yes |
| Positive ID and 0 | Yes |
| Two positive IDs, equal or different | No |
| Two zeros | No |

| Before Filter                                        | After Filter                                        |
|------------------------------------------------------|-----------------------------------------------------|
| ![](Images/ErodeDilateCoordinationNumber_Before.png) | ![](Images/ErodeDilateCoordinationNumber_After.png) |

### How This Filter Works

When the count is positive and meets the selected threshold, the cell receives a selected opposite-status neighbor's tuple. The feature ID and all non-ignored sibling cell arrays follow that donor. Preserve a separate reference; a copied orientation or quality value is an estimate at the target location.

Updates occur in scan order and can affect decisions later in the same pass. This is not a simultaneous morphological operation. Threshold *4* in a single slice targets cells surrounded on all four faces by the opposite status; a threshold above *4* cannot select a cell there. Lower thresholds can change more interfaces.

### Loop Until Gone

By default the filter performs **one** pass. *Loop Until Gone* repeats the scan while qualifying cells remain. Inspect a single pass first: even one pass can erase originally valid pixels as well as fill missing pixels. Repeated execution is not evidence of a physically correct boundary.

### When to Use This Filter

Use this filter to inspect local zero-versus-positive cleanup. An isolated positive grain surrounded by other positive grains will not be removed by this rule. For a feature-size rule, consider [Remove Minimum Size Features](RequireMinimumSizeFeaturesFilter.md); for a scalar-driven donor choice, consider [Replace Element Attributes with Neighbor (Threshold)](ReplaceElementAttributesWithNeighborValuesFilter.md). Neither alternative establishes whether a small feature or missing region is an acquisition artifact.

### Required Input Sources

- **Cell Feature Ids** -- produced by a segmentation filter such as [Segment Features (Misorientation)](../OrientationAnalysis/EBSDSegmentFeaturesFilter.md) or [Segment Features (Scalar)](ScalarSegmentFeaturesFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

+ `T12_Cleanup_Examples/(01) T12 Missing Data Cleanup Comparison.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
