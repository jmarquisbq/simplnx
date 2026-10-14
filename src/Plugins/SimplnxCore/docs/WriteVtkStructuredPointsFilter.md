# Write Vtk Structured Points

## Group (Subgroup)

I/O Filters

## Description

This filter writes the selected data to a **VTK legacy file**, an older Visualization Toolkit format with text headers and either ASCII or binary payloads. Common scientific visualization tools such as **ParaView** and **VisIt** support the format.

The file is written with a dataset type of `STRUCTURED_POINTS`. A structured-points dataset describes a regular grid with uniform spacing along each axis: the grid is fully defined by its origin, its per-axis spacing, and its dimensions, so no explicit coordinate arrays are stored. This makes it the most compact VTK form for image data.

The user selects which **Cell** **Data Array**s from the **Image Geometry** are written into the file.

**Note:** This filter only writes cell data to the VTK file.

The file's `DIMENSIONS` count grid points, so each axis is one larger than the input cell dimension. `ORIGIN` stays at the input lower corner and `SPACING` stays unchanged. Only the selected arrays are transferred; the file does not retain other NX attribute matrices, the pipeline or a length-unit declaration. Keep unit/provenance information and any validity mask with the exported data.

### When to use this filter vs Write Vtk Rectilinear Grid

Use **Write Vtk Structured Points** when the **Image Geometry** has uniform spacing (the usual case) and a compact file is desired. Use [Write Vtk Rectilinear Grid](WriteVtkRectilinearGridFilter.md) when the output must store explicit per-axis coordinate arrays (for example, to interoperate with a tool that expects a `RECTILINEAR_GRID` dataset).

The *Write Binary File* parameter controls the on-disk encoding. When *false* (the default), the data is written as plain ASCII text, which is portable and easy to inspect but larger and slower to read. When *true*, the data is written in VTK's binary form, producing smaller files that load faster.

### Required Input Sources

- **Image Geometry** -- the geometry whose origin, spacing, and dimensions define the grid.
- **Cell Data Array(s) to write** -- any per-cell arrays on the **Image Geometry**. A common choice is a *FeatureIds* array produced by [Segment Features (Scalar)](ScalarSegmentFeaturesFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Bentheimer_Resolution_Examples/(03) Bentheimer Interior Region Comparison.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
