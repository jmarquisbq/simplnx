# Create Surface Mesh (Surface Nets)

## Group (Subgroup)

Surface Meshing (Generation)

## Description

This filter uses the algorithm from {1} to produce a triangle surface mesh. The code is directly based on the sample code from the paper but has been modified to
work with the simplnx library classes. This filter uses a different algorithm that aims to produce a mush that keeps sharp edges
while still producing a mesh superior to marching cubes or QuickMesh.

From the abstract of the paper:

        We extend 3D SurfaceNets to generate surfaces of segmented 3D medical images composed
        of multiple materials represented as indexed labels. Our extension generates smooth, high-
        quality triangle meshes suitable for rendering and tetrahedralization, preserves topology and
        sharp boundaries between materials, guarantees a user-specified accuracy, and is fast enough
        that users can interactively explore the trade-off between accuracy and surface smoothness.

This filter will ensure that the smallest of the 2 **FaceLabel** values will always be in the first component (component[0]). This will allow assumptions made in
downstream filters to continue to work correctly.

This filter attempts to repair the windings for a mesh. This may not be possible due to the nature of how meshes are stored in the software. See Verify Traingle Winding documentation for detailed breakdown of nuance.

---------------

![Example SurfaceNets Output](Images/SurfaceNets_Output.png)

SurfaceNets without the built-in smoothing applied

---------------

![Example SurfaceNets Output](Images/SurfaceNets_Smooth_Output.png)

SurfaceNets output **with** the built-in smoothing operation applied.

---------------

## Node Types

During the meshing process, each vertex, or node, will get a "Node Type" value assigned to it. These will range from 0 to 6. The value is an internal representation from the SurfaceNets algorithm. They are roughly equivelent to the Node Types from the Quick Surface Mesh algorithm but not strictly the same.

- Node Type = 0: This is a node that ONLY has 2 features connected to the node.
- Node Type = 2: This is a node that has 3 features connected to the node, such as a triple line
- Node Type = 3-6: These nodes have 4 or more features connected to the node.


| Node Type | Example Image                                |
|-----------|----------------------------------------------|
| 0 | ![Node Type 0](Images/SurfaceNets_NodeType_0.png)|
| 2 |  ![Node Type 2](Images/SurfaceNets_NodeType_2.png)|
| 3 |  ![Node Type 3](Images/SurfaceNets_NodeType_3.png)|
| 4 | ![Node Type 4](Images/SurfaceNets_NodeType_4.png)|
| 6 |  ![Node Type 6](Images/SurfaceNets_NodeType_6.png)|

### Exterior or Boundary Nodes

Nodes that appear on the exterior of a volume have Node Type values starting at 10 and going up from there. For instance, a triple line that is also on the exterior of the volume should have a value of 12.

![Exterior Node Types](Images/SurfaceNets_NodeType_Exterior.png)

### Exterior or Boundary Triangles

Each triangle that is created will have an 2 component attribute called `Face Labels` that represent the Feature ID on either
side of the triangle. If one of the triangles represents the border of the virtual box then one of the FaceLables will
have a value of -1.

## Algorithm

This filter uses a dispatch mechanism to select the optimal algorithm implementation based on the storage type of the input arrays.

### In-Core Algorithm (Direct)

When all input arrays are backed by in-memory storage, the **SurfaceNetsDirect** algorithm is used. This delegates to the MMSurfaceNet library, which is a C++ implementation of the Surface Nets algorithm from Frisken (2022).

The algorithm proceeds in six phases:

1. **Build Surface Net**: The MMSurfaceNet library constructs a padded grid (dimX+2, dimY+2, dimZ+2) and classifies every cell by examining its 8 corner labels. Cells where not all corners have the same FeatureId are "surface cells" and receive a mesh vertex at the cell center. This reads the entire FeatureIds array via direct element access.

2. **Smoothing** (optional): Iterative Laplacian-like relaxation moves each vertex toward the average of its face-connected neighbors, clamped to stay within `MaxDistanceFromVoxel` of the cell center. The `RelaxationFactor` controls the blending between current and average position.

3. **Vertex Transformation**: Converts cell-local coordinates (where 0.5 = cell center) to world coordinates using the ImageGeom origin and spacing.

4. **Triangle Counting**: First pass over surface vertices, checking 3 edges per cell (BackBottom, LeftBottom, LeftBack) for feature boundary crossings. Each crossing produces a quad (4 vertices) that becomes 2 triangles.

5. **Triangle Generation**: Second pass that writes triangle connectivity and face labels. Quads are triangulated using the diagonal that minimizes total triangle area, reducing self-intersections.

6. **Winding Repair** (optional): Fixes inconsistent triangle orientations.

### Out-of-Core Algorithm (Scanline)

When any backend-capable input or created output uses chunked out-of-core (OOC) storage, the **SurfaceNetsScanline** algorithm is selected automatically. This variant reimplements the entire Surface Nets algorithm without the MMSurfaceNet library using disk-backed fixed records and bounded working sets.

Key optimizations:

- **Z-slice bulk I/O**: FeatureIds are read two Z-slices at a time via `copyIntoBuffer()` with a rolling ping-pong buffer. Each cell's 8 corner labels are resolved from the two buffered slices using a `cornerLabel()` helper.

- **Disk-backed padded records**: One fixed `SurfaceCellRecord` per padded cell is held by the registered temporary-record provider. A bounded page cache supplies neighbor lookup and mutable smoothing/node-type state without resident cell or mesh staging.

- **Bounded smoothing**: In-place Gauss-Seidel relaxation reads and writes cached records in raster order, preserving the Direct algorithm's clamp behavior.

- **Chunked output and transfers**: Vertices, node types, connectivity, labels, and selected transfer arrays are emitted in fixed chunks. FeatureIds and transfer sources use bounded pages; no cell-level singleton access is used in the OOC path.

The optional **Repair Triangle Winding** stage currently uses the shared in-core triangle-connectivity utility after mesh generation. The bounded external replacement for that common stage is tracked separately because it is also used by Quick Surface Mesh and M3C Surface Meshing.

### Performance

The in-core (Direct) variant is fastest for datasets that fit in memory, leveraging the optimized MMSurfaceNet library. The out-of-core (Scanline) variant avoids resident cell-sized intermediates and per-element reads that would cause chunk thrashing on OOC datasets. Both variants produce identical output.

## Notes

This filter should be used in place of the "QuickMesh Surface Filter".


## Comparison of Surface Meshing Filters

DREAM3D-NX provides three **Filters** that convert a segmented grid into a multi-material triangle surface mesh. All three produce the same output data model (a **Triangle Geometry** with **Face Labels** and **Node Types**), so they are interchangeable inputs to downstream mesh **Filters**; they differ in how the triangles are generated and therefore in mesh smoothness, triangle count, and performance.

| Aspect | Create Surface Mesh (QuickMesh) | Create Surface Mesh (Surface Nets) | Create Surface Mesh (M3C) |
|---|---|---|---|
| Algorithm | Voxel-face ("staircase") | Dual (SurfaceNets) | Primal multi-material marching cubes |
| Vertex placement | Voxel corners | One relaxed vertex per boundary **Cell** | On **Cell** edges/faces |
| Surface quality | Blocky / stair-stepped | Smooth, sharp edges preserved | Faceted (marching-cubes) |
| Built-in smoothing | No (apply Laplacian Smoothing afterward) | Yes, optional and accuracy-controlled | No (apply Laplacian Smoothing afterward) |
| Relative triangle count | Highest | Lowest | Moderate to high (configuration dependent) |
| Multi-material junctions | Yes | Native | Yes (via case table) |
| Performance | Fastest | Fast (parallelized) | Moderate (multithreaded) |
| Status | Deprecated | Recommended default | Specialized / legacy-compatible |

**Guidance:** Surface Nets is the recommended default for most workflows — it yields the smoothest mesh with the fewest triangles and preserves sharp boundaries. Use M3C when a primal, marching-cubes case-table topology is required for a specific downstream modeling or simulation workflow. QuickMesh is retained for backward compatibility.

% Auto generated parameter table will be inserted here

## Example Pipelines

        Pipelines/SimplnxCore/SurfaceNets_Demo.d3dpipeline

## Citations

{1}[SurfaceNets for Multi-Label Segmentations with Preservation of Sharp Boundaries](https://jcgt.org/published/0011/01/03/paper.pdf)

## License & Copyright

`Sarah F. Frisken, SurfaceNets for Multi-Label Segmentations with Preservation of Sharp
Boundaries, Journal of Computer Graphics Techniques (JCGT), vol. 11, no. 1, 34–54, 2022`
[http://jcgt.org/published/0011/01/03](http://jcgt.org/published/0011/01/03)

## DREAM3D-NX Mailing Lists

If you need more help with a **Filter**, please consider asking your question on
the [DREAM3D-NX Users Google group!](https://groups.google.com/forum/?hl=en#!forum/dream3d-users)
