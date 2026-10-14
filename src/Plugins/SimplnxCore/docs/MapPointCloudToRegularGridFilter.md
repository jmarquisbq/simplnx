# Map Point Cloud to Regular Grid

## Group (Subgroup)

Sampling (Mapping)

## Description

This **Filter** determines, for a user-defined grid, in which voxel each point in a **Vertex Geometry** lies.  The user can either construct a sampling grid by specifying the dimensions, or select a pre-existing **Image Geometry** to use as the sampling grid.  The voxel indices that each point lies in are stored on the vertices.

Additionally, the user may opt to use a mask; points for which the mask is false are ignored when computing voxel indices (instead, they are initialized to voxel 0).

The per-vertex indices identify which grid cell contains each point; they do not transfer point attributes to cells. [Interpolate Point Cloud to Regular Grid](InterpolatePointCloudToRegularGridFilter.md) transfers scalar attributes, but its current version computes indices directly from coordinates and does not consume this array. Mapping can still provide an ownership record and an explicit out-of-bounds check before interpolation.

### Sampling Grid Type

The *Sampling Grid Type* parameter controls how the target grid is defined:

- **Manual [0]**: The user specifies the grid dimensions directly as voxel counts along X, Y, and Z (dimensionless counts). The filter derives a new **Image Geometry** from the selected point bounds. Its fitted dimensions can differ from the requested counts. Use an existing geometry when exact origin, spacing, dimensions, and units must be preserved.
- **Use Existing Image Geometry [1]**: The user selects a pre-existing **Image Geometry** from the data structure to use as the sampling grid.

### Out of Bounds Handling

The *Out of Bounds Handling* parameter provides the following choices:

- **Silent [0]**: Silently uses the user-supplied out-of-bounds value. This is the default.
- **Warning with Count [1]**: Emits a filter warning after execution containing the number of out-of-bounds values encountered.
- **Error at First Instance [2]**: Emits a filter error at the first out-of-bounds value encountered.

The default selection is `Silent`, but it is mostly provided as a way to preserve existing functionality. What follows are a few use cases we had in mind when adding this functionality, organized by handling type:

- `Silent` option:
  - User may want to preserve identical functionality between **SIMPL** and **simplnx**
  - User may expect values to fall outside the target image geometry or intend to crop all that fall outside it anyway
- `Warning with Count` option:
  - User may be intending to create a general use pipeline for various different tasks, for which monitoring and validation may be important
  - User may intend to create a workflow that will be distributed in which the end user may not have control over the parameter, but should be monitoring for anomalies in output
  - User may want to watch for unexpected behavior
- `Error at First Instance` option
  - User may want to trace down where an anomaly first occurred
  - User may be creating a pipeline in a known problem space with a well defined outcome where any data anomalies must be caught early to prevent downstream problems

Continuing along the Out-of-Bounds discussion, the Out-of-Bounds value allows the user to specify a specific `uint64` (0 - 18,446,744,073,709,551,615) value to use for every value from the vertex geometry that falls outside the image geometry. The default value is just the max `unsigned long long int` in an effort to make sure that it doesn't intersect with existing indexed values. This is identical to previous functionality. However, consider the situation where a user has a geometry that contains 1000 voxels, in this case the actual index values are 0-999, so a user could select 1000 and it wouldn't overlap any existing voxel index. Doing this may reduce skew of coloring or other statistic-based analysis. Advanced users may intentionally select a value that overlaps an existing voxel index they wish to remove in a later filter or to later downcast the datasize without overflow, but this is considered an edge case that is functional, but not recommended.

### Required Input Sources

- **Vertex Geometry** -- the input point cloud whose points are mapped to grid voxels, typically created by an import step or a filter that produces a **Vertex Geometry**.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Small_IN100_Grain_Surface_Examples/(04) Small IN100 Point Cloud Interchange.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
