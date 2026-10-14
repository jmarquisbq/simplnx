# Compute Feature Face Misorientation (Face)

## Group (Subgroup)

Processing (Crystallography)

## Description

This **Filter** computes the symmetry-reduced misorientation angle between the two **Features** adjoining each **Triangle** in a **Triangle Geometry**. Misorientation describes how far one crystal orientation must rotate to match the other, accounting for the phase's crystal symmetry.

The current version stores one float32 scalar per face, in **degrees**. It does not produce RGB colors, a three-component axis-times-angle vector, or a four-component axis-angle array. The former *Store Full Axis Angle* option is not present. Pipelines using parameter version 1 must replace `surface_mesh_face_misorientation_colors_array_name` with `misorientation_array_name` and update downstream consumers for the scalar result.

Faces are evaluated when both adjoining features have the same positive phase and a supported crystal structure. Exterior/background faces and phase-mismatched faces receive NaN, not a zero-angle observation. Exclude these values from summaries. Average orientations must use the same feature-ID namespace as the face labels; twin parent IDs cannot index a child-feature table.

### Required Input Sources

- **Face Labels** -- produced by [Surface Nets](../SimplnxCore/SurfaceNetsFilter.md) or another labeled surface mesher.
- **Average Quaternions** -- produced by [Compute Average Orientations](ComputeAvgOrientationsFilter.md).
- **Feature Phases** -- produced by [Compute Feature Phases](../SimplnxCore/ComputeFeaturePhasesFilter.md).
- **Crystal Structures** -- read from EBSD data or supplied by [Create Ensemble Info](CreateEnsembleInfoFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)

+ (07) Small IN100 Mesh Statistics

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
