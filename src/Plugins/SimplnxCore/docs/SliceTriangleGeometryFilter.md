# Slice Triangle Geometry

## Group (Subgroup)

Sampling (Geometry)

## Description

This **Filter** intersects an input **Triangle Geometry** with planes perpendicular to the Z axis, producing an **Edge Geometry**. The user controls the range and spacing of the planes. The output edges carry zero-based slice IDs. The created slice attribute matrix does not currently contain computed area or perimeter arrays; those measurements require additional processing.

Both the *Slice Spacing* and the *User Defined Range* values use the input coordinate units (for example, micrometers for a mesh calibrated in micrometers). Use positive spacing and inspect planes that coincide with vertices or horizontal faces. Current output geometry does not retain the input length-unit metadata ([issue #1796](https://github.com/BlueQuartzSoftware/simplnx/issues/1796)); preserve the coordinate numbers and explicitly restore the intended units before interpreting the result.

Additionally, if the input **Triangle Geometry** is labeled with an identifier array (such as different regions or features), the user may select this array and the resulting edges will inherit these identifiers.

## Example Output

Example Surface Mesh being sliced with a 0.25 slice spacing.

![](Images/SliceTriangleGeometry_1.png)


Example Surface Mesh being sliced with a 2.0 slice spacing.

![](Images/SliceTriangleGeometry_2.png)

### Slice Range

The *Slice Range* parameter controls which portion of the geometry is sliced:

- **Full Range [0]**: Slices across the entire extent of the geometry along the slicing direction.
- **User Defined Range [1]**: Allows specifying custom start and end values (in the geometry's physical units) for the slicing range, restricting slices to a subregion of the geometry.

### Required Input Sources

- **Triangle Geometry** -- an existing surface mesh. Typically produced by [Create Surface Mesh (QuickMesh)](QuickSurfaceMeshFilter.md), read from disk via [Read STL File](ReadStlFileFilter.md), or assembled from multiple files by [Combine STL Files](CombineStlFilesFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

CreateScanVectors

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `NIST_AMBench_2018_Examples/(01) NIST Bridge Contour Inspection.d3dpipeline` (SimplnxCore)

## License & Copyright 

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
