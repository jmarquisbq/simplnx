# Sample Triangle Geometry on Uncertain Regular Grid

## Group (Subgroup)

Sampling (Resolution)

## Description

This **Filter** samples a triangulated surface mesh at perturbed positions associated with a nominal uniform **Image Geometry**. The uncertainty simulates bounded positioning error of a sampling probe. A **Cell** is one nominal grid volume element, and a **Feature** is one labeled region of the surface mesh. The bounds are physical lengths in the mesh coordinate units, not dimensionless fractions or calibrated confidence limits.

The user specifies the number of **Cells** along the X, Y, and Z directions, plus the resolution (spacing) and origin that define the grid. The grid resolution and origin are given in the same physical length units as the input surface mesh (typically microns). The three uncertainty values are also physical lengths in those same units. The sampling, with uncertainty, is performed by the following steps:

1. Determine the bounding box and **Triangle** list of each **Feature** by scanning all **Triangles** and noting the **Features** on either side of each **Triangle**.
2. Perturb nominal cell-center coordinates using uniform draws in [-1, 1] multiplied by the uncertainty bounds. Draw X separately per point, Y once per complete row, and Z once per complete slice. The resulting offsets are correlated within rows and slices, rather than independent in every coordinate at every cell.
3. For each perturbed **Cell**, determine which bounding box(es) it falls in (*Note:* the bounding boxes of multiple **Features** can overlap).
4. For each bounding box a **Cell** falls in, test it against that **Feature's** **Triangle** list to determine whether the **Cell** lies within that n-sided polyhedron. (*Note:* if the surface mesh is **conformal** -- meaning adjacent features share exactly one common surface with no gaps or overlaps -- each **Cell** belongs to only one **Feature**; if not, the last **Feature** the **Cell** is found inside of *owns* the **Cell**.)
5. Assign the **Feature** number that the **Cell** falls within to the *Feature Ids* array in the new rectilinear grid geometry.

**Note that the unperturbed grid is where the *Feature Ids* actually live, but the perturbed locations are where the Cells are sampled from. Essentially, the *Feature Ids* are stored where the user *thinks* the sampling took place, not where it actually took place.**

For the variant that samples onto a regular grid without positional uncertainty, see the sibling filter [Sample Triangle Geometry on Regular Grid](RegularGridSampleSurfaceMeshFilter.md), which shares the same grid (dimensions, origin, spacing) parameters.

Enable the fixed-seed option for repeatable comparisons in the same implementation. The saved seed does not establish that a chosen uncertainty bound represents an instrument's actual error distribution.

### Current output metadata limitation

[Issue #1800](https://github.com/BlueQuartzSoftware/simplnx/issues/1800) tracks a wrong cell-matrix association and XYZ rather than ZYX label tuple dimensions on non-cubic grids. The Small IN100 companion reconstructs a correctly associated image and copies the unchanged labels on its cubic reference grid. Check both the geometry's designated cell matrix and array tuple order before using another grid shape.

### Required Input Sources

- **Triangle Geometry** -- the surface mesh to sample, typically produced by a surface-meshing filter such as [Create Surface Mesh (Surface Nets)](SurfaceNetsFilter.md) or [Create Surface Mesh (QuickMesh)](QuickSurfaceMeshFilter.md).
- **Face Labels** -- the per-face 2-component array identifying the **Features** on either side of each triangle, produced by the same surface-meshing filter.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
