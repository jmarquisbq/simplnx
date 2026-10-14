# Copy Data Object

## Group (Subgroup)

Core (Generation)

## Description

This **Filter** deep copies one or more DataObjects.

**In the case of copying _DataObject_s that inherit from _BaseGroup_**, such as _DataGroup_ or _AttributeMatrix_, **it will copy all of the child objects recursively**, that is to say all of an object's children and childrens' children and so on will be copied if applicable.

Commonly used _BaseGroup_ children:

- **_ALL_** Geometries
- _DataGroup_
- _AttributeMatrix_
- _GridMontage_

See the DataStructure section of the reference manual for a complete hierarchy.

When *Copy to New Parent* is toggled on, a new parameter appears. This parameter, *Copied Parent Group*, lets the selected objects be copied into a different parent **DataGroup**, **Attribute Matrix**, or **Geometry** rather than alongside their originals.

### Required Input Sources

- **Objects to Copy** -- one or more existing **DataObjects** in the Data Structure.
- **Copied Parent Group** (only when *Copy to New Parent* is enabled) -- an existing parent container where the copies will be placed.

% Auto generated parameter table will be inserted here

## Example Pipelines

+ `T12_Cleanup_Examples/(01) T12 Missing Data Cleanup Comparison.d3dpipeline` (OrientationAnalysis)

+ `T12_Orientation_Examples/(02) T12 Orientation Interchange.d3dpipeline` (OrientationAnalysis)

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Cambridge_Ti_CAxis_Examples/(01) Cambridge Ti Import and C-Axis Directions.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(06) Small IN100 Neighbor Relationships.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(07) Small IN100 Bounded Voxel Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(02) Small IN100 Surface Smoothing.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(03) Small IN100 Surface Transform and Export.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(01) Small IN100 Complete Grain Surface.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(03) T12 Compact Report.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(03) Small IN100 Grain Point Cloud Registration.d3dpipeline` (OrientationAnalysis)

+ `Ti64_Segmentation_Examples/(03) Ti64 Minimum Size Cleanup.d3dpipeline` (ImageProcessing)

+ `Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline` (OrientationAnalysis)
+ `Ti64_Image_Cleanup_Examples/(03) Ti64 Threshold Sensitivity.d3dpipeline` (ImageProcessing)

+ `NIST_AMBench_2018_Examples/(01) NIST Bridge Contour Inspection.d3dpipeline` (SimplnxCore)

+ `T12_Import_Examples/(01) T12 Typed CSV Import.d3dpipeline` (OrientationAnalysis)

+ `Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline` (OrientationAnalysis)

+ `NIST_AMBench_2018_Examples/(03) NIST Bridge STL Assembly.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
