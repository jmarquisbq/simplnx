# Pad Image Geometry

## Group (Subgroup)

Generic (Generic)

## Description

This **Filter** extends an **Image Geometry** outward by adding cells around the edges. Each added cell is initialized to a user-specified default value. Optionally, the geometry's origin can be updated so that the original data stays in the same physical location after padding.

Padding is the inverse operation of [Crop Geometry (Image)](CropImageGeometryFilter.md). Common use cases are creating a margin around a sample before applying a transformation, or extending a small ROI to a standard size for batch processing.

### Example

Given the input geometry in Figure 1:

#### Figure 1

![](Images/PadImageGeometry_0.png)

Padding with X Min=0, X Max=10, Y Min=0, Y Max=10 produces:

#### Figure 2

![](Images/PadImageGeometry_1.png)

### Pad Amounts and Units

The pad amounts (*X Min*, *X Max*, *Y Min*, *Y Max*, *Z Min*, *Z Max*) are integers in **cells/voxels**. A value of 10 adds 10 cells to that face. Setting a pad amount to 0 means no padding on that face.

### Default Value

Each padded cell is initialized to the *Default Value* for every cell-level **Attribute Array** in the target Attribute Matrix. The default value is interpreted in the same data type as each array (e.g., for a uint8 array, the value is cast to uint8; for a float32 array, to float32). Pick a value that is sentinel-like in every array (commonly 0).

### Update Origin

If *Update Origin* is **OFF** (the default), the geometry's lower-corner origin stays fixed. The original cells move to higher indices and therefore shift physically by the lower-side pad count times spacing on each enabled axis. The new low-side cells occupy the beginning of the unchanged coordinate range; they are not placed below the unchanged origin.

If *Update Origin* is **ON**, the geometry's origin is shifted so that the original data stays in its original physical coordinates. New cells extend to coordinates below the original origin.

Padding fills every component of every cell array with the same default value outside the original region. A zero-filled coordinate array is not a valid position map for the new cells; remove it from a working copy and regenerate coordinates after padding with [Compute Coordinates/Indices Array From Image Geom](ComputeCoordinatesImageGeomFilter.md). Preserve an explicit support mask when fill values might also be valid measurements. Padding does not extend the measured field of view.

### Required Input Sources

- **Input Image Geometry** -- the geometry to extend. Typically produced by [Create Image Geometry](CreateImageGeometryFilter.md), [Read Image Stack](../ImageProcessing/ReadImageStackFilter.md), or an EBSD reader.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Pad_Image_Geometry.d3dpipeline`

+ `Bentheimer_Resolution_Examples/(03) Bentheimer Interior Region Comparison.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
