# Compute Feature Phases Binary

## Group (Subgroup)

Generic (Misc)

## Description

This **Filter** assigns a dimensionless binary class, 0 or 1, to each **Feature** using a per-**Cell** mask and the cell's **Feature Id**. A true or nonzero mask value assigns class 1; a false or zero value assigns class 0. The result is indexed by Feature Id, not by cell position. A mask alone does not establish a material phase: selected/unselected brightness classes are equally valid uses.

Use this filter when the mask is constant within each feature. For example, segment an integer 0/1 image with [Segment Features (Scalar)](ScalarSegmentFeaturesFilter.md), tolerance *0*, and no mask; both classes receive connected-component labels. This filter then records which class each component belongs to before [Remove/Extract Flagged Features](RemoveFlaggedFeaturesFilter.md) selects the desired class.

Cells are processed in stored order. If one feature contains both true and false mask values, the **last visited cell wins**. The result is neither a majority vote nor the fraction of selected cells. Use [Compute Array Statistics](ComputeArrayStatisticsFilter.md) on a numeric 0/1 mask when a within-feature selected fraction is needed.

Despite the parameter label *Cell Data Attribute Matrix*, choose the **Feature Attribute Matrix** paired with the input Feature Ids. Its tuple count determines the output size and must include every referenced feature ID. Feature row zero is reserved by conventional segmentation workflows; exclude it from feature reports. Unreferenced output rows retain their initialized values.

### Required Input Sources

- **Cell Feature Ids** -- produced by [Segment Features (Scalar)](ScalarSegmentFeaturesFilter.md) or another segmentation filter. Keep these IDs paired with their feature table.
- **Mask** -- a boolean or uint8 selection, for example from [Multi-Threshold Objects](MultiThresholdObjectsFilter.md) or [Binary Threshold Image](../ImageProcessing/BinaryThresholdImageFilter.md). Values must describe the same cells as the Feature Ids.
- **Feature Attribute Matrix** -- created by the segmentation filter or by indexed [Compute Array Statistics](ComputeArrayStatisticsFilter.md), with a row for every referenced Feature Id.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Ti64_Segmentation_Examples/(03) Ti64 Minimum Size Cleanup.d3dpipeline` (ImageProcessing)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
