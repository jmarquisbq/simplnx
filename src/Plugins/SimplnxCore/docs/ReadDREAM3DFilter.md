# Read DREAM3D-NX File

## Group (Subgroup)

IO (Input)

## Description

This filter reads data from a `.dream3d` file into the pipeline. A `.dream3d` file is an HDF5-based file (HDF5 is a portable, self-describing container format for large scientific datasets) that stores a complete DREAM3D-NX **DataStructure** — its geometries, **Attribute Matrix** groups, and **Data Array**s.

### Selective (Partial) Import

The filter does not have to read the entire file. Through the *Import File Path* parameter, the user can browse the structure of the selected `.dream3d` file and choose exactly which data objects to import. Selecting only the objects that are needed avoids loading large arrays that the rest of the pipeline does not use, which saves memory and time. To import everything, simply select all objects.

### How Imported Objects Are Added

The selected objects are added into the pipeline's current **DataStructure** at the same paths they had in the file. If an object being imported has the same path as an object already present in the current **DataStructure**, the existing object is replaced (overwritten) by the imported one, so take care when reading into a non-empty **DataStructure**. Any pipeline metadata stored in the file may also be read when the file is opened.

### Legacy Files

This filter can also read **legacy** `.dream3d` files — those written by the older DREAM3D 6.x / SIMPL-era applications. Such files use an earlier internal layout, and this filter converts them to the current **DataStructure** format on import.

### Required Input Sources

None — this filter reads directly from a `.dream3d` file on disk.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Ti64_Image_Cleanup_Examples/(02) Ti64 Smoothing Comparison.d3dpipeline` (ImageProcessing)
+ `Ti64_Image_Cleanup_Examples/(03) Ti64 Threshold Sensitivity.d3dpipeline` (ImageProcessing)
+ `Ti64_Segmentation_Examples/(01) Ti64 Connected Bright Regions.d3dpipeline` (ImageProcessing)
+ `Ti64_Segmentation_Examples/(02) Ti64 Connectivity Comparison.d3dpipeline` (ImageProcessing)
+ `Ti64_Segmentation_Examples/(03) Ti64 Minimum Size Cleanup.d3dpipeline` (ImageProcessing)
+ `Cambridge_Ti_CAxis_Examples/(02) Cambridge Ti Grain C-Axis Metrics.d3dpipeline` (OrientationAnalysis)
+ `Cambridge_Ti_CAxis_Examples/(03) Cambridge Ti C-Axis Grouping Comparison.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Boundary_Examples/(01) Small IN100 Boundary and Section Measurements.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(02) Small IN100 Feature Measurements.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(03) Small IN100 Surface Selection Comparison.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(04) Small IN100 Minimum Size Comparison.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(05) Small IN100 Combined Selection.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(06) Small IN100 Neighbor Relationships.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(07) Small IN100 Bounded Voxel Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(02) Small IN100 Surface Smoothing.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(03) Small IN100 Surface Transform and Export.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(01) Small IN100 Complete Grain Surface.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Slip_Examples/(01) Small IN100 Neighbor Orientation and Slip.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Spatial_Examples/(01) Small IN100 Feature Bounds.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Spatial_Examples/(02) Small IN100 Feature and Box Quality.d3dpipeline` (OrientationAnalysis)
+ `T12_Import_Examples/(02) T12 Selective DREAM3D Import.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(01) T12 Derived Reporting Fields.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(02) T12 Feature to Cell Mapping.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(03) T12 Compact Report.d3dpipeline` (OrientationAnalysis)
+ `Bentheimer_Resolution_Examples/(02) Bentheimer Common Grid Comparison.d3dpipeline` (SimplnxCore)
+ `Bentheimer_Resolution_Examples/(03) Bentheimer Interior Region Comparison.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
