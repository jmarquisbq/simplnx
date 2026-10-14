# Compute Vertex to Triangle Distances

## Group (Subgroup)

Sampling (Spatial)

## Description

This **Filter** computes distances between points in a **Vertex Geometry** and triangles in a **Triangle Geometry**.  Specifically, for each point in the **Vertex Geometry**, the Euclidean distance to the closest triangle in the **Triangle Geometry** is stored.  This distance is *signed*: if the point lies on the side of the triangle to which the triangle normal points, then the distance is positive; otherwise, the distance is negative. Additionally, the ID of the closest triangle is stored for each point.

The computed distance is reported in the same length unit as the coordinates of the two input geometries (e.g., microns, millimeters); both geometries must share the same unit system for the result to be meaningful.

The sign of each distance depends on the **Triangle Normals**. For the sign to be correct, the normals must be present and consistently oriented across the mesh (all pointing to the same "outside"). Inconsistent or flipped normals will produce sign errors even when the distance magnitude is correct.

This sign describes the side of the selected nearest face. It is not a general solid-containment test for an open, non-manifold or inconsistently oriented mesh. After [Reverse Triangle Winding](ReverseTriangleWindingFilter.md), recompute normals before repeating the distance calculation. Closest-triangle IDs are row indices in the selected target mesh; edge or vertex ties may admit more than one nearest triangle.

### Required Input Sources

- **Vertex Geometry** -- the point cloud whose distances are measured.
- **Triangle Geometry** -- the surface mesh measured against, typically produced by a surface-meshing filter such as [Quick Surface Mesh](QuickSurfaceMeshFilter.md) or read from a CAD mesh via [Read STL File](ReadStlFileFilter.md).
- **Triangle Normals** -- per-face normals on the **Triangle Geometry**, produced by [Compute Triangle Normals](TriangleNormalFilter.md); required for correct distance signs.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `NIST_AMBench_2018_Examples/(01) NIST Bridge Contour Inspection.d3dpipeline` (SimplnxCore)

+ `Small_IN100_Grain_Surface_Examples/(03) Small IN100 Grain Point Cloud Registration.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
