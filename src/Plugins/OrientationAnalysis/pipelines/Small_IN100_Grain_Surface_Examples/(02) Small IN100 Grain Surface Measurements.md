# Small IN100 grain surface measurements

Executable pipeline: `(02) Small IN100 Grain Surface Measurements.d3dpipeline`

> **Before adapting:** **Validation warning:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

## At a glance

- **Input:** The complete, winding-repaired [grain 2205 shell](%2801%29%20Small%20IN100%20Complete%20Grain%20Surface.md).
- **Question:** What volume does the closed shell enclose, where is the mean of its unique surface vertices, and how does local region 1 map back to original grain 2205?
- **Output:** `SmallIN100_Grain2205_Measurements.dream3d` and a scalar feature CSV with the explicit local-to-original ID map.
- **Run order:** Archive → Feature Preparation → Surface Mesh → Complete Grain Surface → this pipeline. See [README](README.md) for paths and commands.

## Purpose and real-world setting

The measured-derived Small IN100 grain boundary becomes a standalone geometric object in stage 01. This stage evaluates a small set of physical descriptors while retaining the original grain identity. The setting follows the reconstructed three-dimensional EBSD analysis described by [Groeber and Jackson (2014)](https://doi.org/10.1186/2193-9772-3-5), but the selected grain, modern mesh, and numerical outputs are illustrative rather than a reproduction of a reported paper result.

Use these measurements only after confirming that the extracted surface is complete, closed, consistently wound, and calibrated. The mesh is built from source cells spaced 0.25 µm in each direction; its coordinates remain in the source specimen frame.

## Data flow and choices

`CreateDataArray` allocates two-component int32 `LocalSurfaceLabels`. `InitializeData` fills every face with `[-1,1]`: local exterior is component 0 and the single enclosed grain is component 1. A single scalar fill string in `CreateDataArray` cannot express this pair. The original two-component `OriginalFaceLabels` is retained unchanged because it identifies original neighbors, but those neighbors now form *open patches* on the isolated mesh. Using them for closed-volume integration would report misleading results.

The pipeline creates a two-row `Grain Measurements` matrix and an `OriginalFeatureIds` lookup. Row 0 is unused local exterior (`0`); row 1 maps to original reconstructed ID `2205`. This map prevents a local connected-component number from being mistaken for the source segmentation ID. `ComputeTriangleGeomVolumes` integrates the repaired shell with local labels and stores `ClosedSurfaceVolumes`. `ComputeTriangleGeomCentroids` stores `SurfaceVertexMeans`; current code averages each distinct used vertex once. It is **not** an enclosed-volume centroid, area-weighted surface centroid, or source-voxel center of mass. `WriteFeatureDataCSV` exports the scalar measurement rows, and `WriteDREAM3D` keeps labels and full face-level geometry for audit.

## Outputs and interpretation

`Data/Output/Small_IN100_Grain_Surface_Examples/Measurements/SmallIN100_Grain2205_Measurements.dream3d` contains `Grain2205/Face Data/LocalSurfaceLabels` and the two-row `Grain2205/Grain Measurements/{OriginalFeatureIds,ClosedSurfaceVolumes,SurfaceVertexMeans}` arrays along with the original identity arrays, final face areas/normals, and source crop. The CSV at the same path stem writes a count line, header, and **one** data row for `Feature_ID` 1; the CSV writer omits the unused row 0. Read its `OriginalFeatureIds` field to recover source ID 2205.

![Small IN100 grain surface measurements](SmallIN100GrainMeasurements.png)

For this saved source, occupied local row 1 encloses approximately 2.65625 µm³, the 170-voxel source grain's volume (`170 × 0.25³ µm³`); the native float32 result is 2.6562402 µm³. The unique-vertex mean is approximately `(25.2471, 26.5811, 15.4629)` µm. Face area sums to 15.875 µm². These values describe this measured-derived reconstruction and the current mesh. Similar volume does not prove identical boundary topology or an unbiased shape estimator.

## Adapting and checking the workflow

If the source contains multiple disconnected shells, cavities, or open surfaces, decide how each local region should map to original features before reusing `[-1,1]`; this fixed pair is designed for one outward-oriented closed grain. Update the `OriginalFeatureIds` mapping and the row count together. Recheck shell edge incidence, connectedness, winding, nonzero face areas, and signed volume independently. The volume filter stores the absolute result, so a positive value alone does not certify outward winding. Compare its output to a separately computed source-voxel volume only when the original feature is completely retained and the meshing method is known to preserve that volume.

Alternative measurements serve different questions. A source-voxel centroid integrates the segmented volume; an area centroid weights triangle areas; this filter reports a mean of unique surface vertices. Before using `ComputeShapesTriangleGeom`, establish the centroid definition required by its moment calculation and validate the resulting shape axes; `SurfaceVertexMeans` is not a mass centroid. The public [Small IN100 archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) supplies measured sections; the article is CC BY 2.0, while separate archive redistribution terms have not been established. No volume file is packaged here. For an LLM or MCP assistant, use the `.d3dpipeline` for exact paths and arguments, and ask whether the surface is independently closed and which centroid definition the user needs.
