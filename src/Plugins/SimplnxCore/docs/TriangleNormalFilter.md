# Compute Triangle Normals

## Group (Subgroup)

Surface Meshing (Misc)

## Description

This filter computes the **normal** of each **Triangle** in a **Triangle Geometry** (a surface mesh built from triangles) and stores the result as a per-triangle **Face Data** array. A normal is the direction that points straight out of the flat triangle surface.

### How It Is Computed

For a triangle with corner **vertices** *point1*, *point2*, and *point3*, the filter forms two edge vectors

    U = point2 - point1
    V = point3 - point1

and computes their **cross product** (a vector perpendicular to both edges, and therefore perpendicular to the triangle):

    Nx = Uy*Vz - Uz*Vy
    Ny = Uz*Vx - Ux*Vz
    Nz = Ux*Vy - Uy*Vx

The result is then normalized to unit length.

### Output and Direction

The output is a 3-component **unit vector** (length 1) and is therefore **dimensionless**. Its sense (which of the two opposite directions it points along) is set by the **winding order** -- the order in which the triangle's three vertices are listed. Reversing the vertex order flips the normal to point the opposite way. Consistent winding across the mesh is what makes all normals point consistently inward or outward.

### Required Input Sources

- **Triangle Geometry** -- a surface mesh, typically produced by a surface-meshing filter such as [Create Surface Mesh (Surface Nets)](SurfaceNetsFilter.md) or [Create Surface Mesh (QuickMesh)](QuickSurfaceMeshFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

+ `Bentheimer_Resolution_Examples/(04) Bentheimer VTK Interchange.d3dpipeline` (SimplnxCore)

+ `Small_IN100_Distribution_Examples/(01) Small IN100 Boundary Distribution Comparison.d3dpipeline` (OrientationAnalysis)

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ (07) Small IN100 Mesh Statistics
+ Triangle_Face_Data_Demo.d3dpipeline
+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(02) Small IN100 Surface Smoothing.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(03) Small IN100 Surface Transform and Export.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(01) Small IN100 Complete Grain Surface.d3dpipeline` (OrientationAnalysis)

+ `NIST_AMBench_2018_Examples/(01) NIST Bridge Contour Inspection.d3dpipeline` (SimplnxCore)

+ `NIST_AMBench_2018_Examples/(03) NIST Bridge STL Assembly.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
