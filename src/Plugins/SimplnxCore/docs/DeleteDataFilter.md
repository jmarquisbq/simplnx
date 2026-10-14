# Delete Data

## Group (Subgroup)

Core (Memory/Management)

## Description

This **Filter** removes one or more **Data Objects** from the **Data Structure**. Common use cases:

- **Free memory.** When subsequent filters need as much RAM as possible and some arrays are no longer needed.
- **Avoid name collisions.** DREAM3DNX does not allow two objects at the same hierarchy level to share the same name. If a downstream filter will create an object with the same name as an existing one, delete the existing one first.
- **Clean up after extraction.** When *Move* or *Extract* options on other filters left behind unused parent containers.

### Cascade Behavior

Deletion is recursive on containers. Deleting a:

- **Geometry** also deletes its child Attribute Matrices and all arrays inside them.
- **Attribute Matrix** also deletes all child arrays.
- **DataGroup** also deletes all of its children, including nested DataGroups, recursively.
- **Data Array** removes only that array.

## WARNING: Downstream Preflight Will Fail

Any filter later in the pipeline that references a deleted object by path will fail preflight when re-run. Delete only objects you are certain are not used downstream, or be prepared to update downstream filter parameters.

### Required Input Sources

- **Data Objects to Delete** -- a list of paths to existing **DataObjects** in the Data Structure.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(06) Small IN100 Neighbor Relationships.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(02) Small IN100 Surface Smoothing.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(01) Small IN100 Complete Grain Surface.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(03) T12 Compact Report.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(03) Small IN100 Grain Point Cloud Registration.d3dpipeline` (OrientationAnalysis)

+ `OIM_Nickel_Pattern_Examples/(01) OIM Nickel Pattern Inspection.d3dpipeline` (OrientationAnalysis)
+ `Bentheimer_Resolution_Examples/(03) Bentheimer Interior Region Comparison.d3dpipeline` (SimplnxCore)

+ `NIST_AMBench_2018_Examples/(01) NIST Bridge Contour Inspection.d3dpipeline` (SimplnxCore)

+ `T12_Reporting_Examples/(02) T12 Feature to Cell Mapping.d3dpipeline` (OrientationAnalysis)

+ `Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
