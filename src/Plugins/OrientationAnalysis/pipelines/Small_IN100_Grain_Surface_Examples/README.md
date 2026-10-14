# Small IN100 complete grain surface examples

These four executable examples isolate one complete measured-derived grain, measure its closed shell, demonstrate point-cloud assembly and registration, and reconstruct a calibrated point cloud from numeric HDF5 arrays with a comparison of observed and interpolated grid support. The `.d3dpipeline` files define the steps and parameters; the matching Markdown explains their use, and the JSON-compatible YAML supports search and generation. The target is original reconstructed `FeatureId` **2205** in the documented Small IN100 interior crop. This small grain has 170 source voxels and remains clear of every crop wall.

## Run order and files

Run the prerequisites and stage 01 from the same writable working directory. Stages 02 and 03 branch independently from stage 01; stage 04 then reads stage 03's saved HDF5 file:

1. `Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline` imports the 117 measured ANG sections from [Small_IN100.tar.gz](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz). The [Small IN100 examples README](../Small_IN100_Examples/README.md#quick-start-117-section-3d-example) explains archive extraction.
2. `Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline` establishes original `FeatureIds` without whole-volume measurement rows in the mesh path.
3. `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` crops to the 24-cubed interior region and writes `Data/Output/Small_IN100_Geometry_Examples/Surface/SmallIN100_SurfaceMesh.dream3d`.
4. [(01) Small IN100 Complete Grain Surface](%2801%29%20Small%20IN100%20Complete%20Grain%20Surface.md) selects every face with `2205` in **either** original `FaceLabels` component, compacts the mesh, and repairs winding. It writes `Data/Output/Small_IN100_Grain_Surface_Examples/Extraction/SmallIN100_Grain2205_Surface.dream3d`.
5. [(02) Small IN100 Grain Surface Measurements](%2802%29%20Small%20IN100%20Grain%20Surface%20Measurements.md) reads stage 01, assigns local shell labels, and writes `Data/Output/Small_IN100_Grain_Surface_Examples/Measurements/SmallIN100_Grain2205_Measurements.{dream3d,csv}`.
6. [(03) Small IN100 Grain Point Cloud Registration](%2803%29%20Small%20IN100%20Grain%20Point%20Cloud%20Registration.md) reads stage 01's source image, partitions 170 grain voxel centers into complementary tiles, reassembles them, and registers a known displaced copy. It writes `Data/Output/Small_IN100_Grain_Surface_Examples/Registration/SmallIN100_Grain2205_PointCloudRegistration.dream3d`; stage 02 is not required.
7. [(04) Small IN100 Point Cloud Interchange](%2804%29%20Small%20IN100%20Point%20Cloud%20Interchange.md) imports selected numeric datasets from stage 03, appends tile rows, splits and recombines coordinate columns, and creates explicit Micrometer vertex geometry. It then compares containing-cell and Gaussian image-quality estimates on two identical 0.5 micrometer grids, retaining contribution counts to distinguish measured occupancy from estimates. It writes `Data/Output/Small_IN100_Grain_Surface_Examples/Interchange/SmallIN100_Grain2205_PointCloudInterchange.dream3d` and `SmallIN100_Grain2205_PointTable.csv` in the same Interchange folder. Stage 03 must exist before stage 04 preflight.

The `Data/...` paths resolve from the CLI process working directory. Create the output folders before running. For example, from a chosen writable folder with the predecessor `Data/Output` tree:

```powershell
$work = 'C:\path\to\writable\SmallIN100Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines' # Parent of the named Small_IN100 folders.
Set-Location $work
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Grain_Surface_Examples/Extraction' | Out-Null
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Grain_Surface_Examples/Measurements' | Out-Null
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Grain_Surface_Examples/Registration' | Out-Null
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Grain_Surface_Examples/Interchange' | Out-Null
$first = Join-Path $examples 'Small_IN100_Grain_Surface_Examples/(01) Small IN100 Complete Grain Surface.d3dpipeline'
$second = Join-Path $examples 'Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline'
$third = Join-Path $examples 'Small_IN100_Grain_Surface_Examples/(03) Small IN100 Grain Point Cloud Registration.d3dpipeline'
$fourth = Join-Path $examples 'Small_IN100_Grain_Surface_Examples/(04) Small IN100 Point Cloud Interchange.d3dpipeline'
& $runner --execute $first
if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $first (exit $LASTEXITCODE)" }
& $runner --execute $second
if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $second (exit $LASTEXITCODE)" }
& $runner --execute $third
if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $third (exit $LASTEXITCODE)" }
& $runner --execute $fourth
if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $fourth (exit $LASTEXITCODE)" }
```

For the GUI, use **File > Open** and set absolute file paths if its working directory differs. No measured volume, intermediate checkpoint, or archive is distributed in this folder. The source archive SHA-512 is `79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b`.

The source is a reconstructed Small IN100 EBSD volume in the setting described by [Groeber and Jackson (2014)](https://doi.org/10.1186/2193-9772-3-5). These modern extraction and repair steps illustrate a complete-grain workflow; they do not reproduce a published numerical result. The article is CC BY 2.0. Separate archive redistribution terms have not been established. `PythonGeneration.yaml` describes generated Python from the native pipelines; generated execution and out-of-core execution require separate verification.

Reference-input expectations are 508 selected faces and 256 used vertices, one closed connected shell, 15.875 square micrometers of surface area, and 2.65625 cubic micrometers enclosed volume. These are audit values for this source and implementation, not acceptance tolerances for other reconstructions. The original `FaceLabels` record neighboring grain identity; `LocalSurfaceLabels` describe the single extracted shell. The measurement called `SurfaceVertexMeans` averages unique mesh vertices and is not a volume centroid.

Stage 03 uses complementary tiles with 89 and 81 points. The tiles and the transformed moving cloud derive from the same reconstruction, so they do not measure agreement between independent acquisitions. `SourceROICellIndex` tracks each point back to a flat tuple in the cropped image; inherited `Image Quality` is scalar metadata, not an ICP weight. Explicit calibrated geometry copies preserve Micrometer units through each checkpoint to work around the [unit-propagation defect](https://github.com/BlueQuartzSoftware/simplnx/issues/1784). Overlapping-tile duplicate removal remains excluded because [duplicate detection misses identical points on tied grid planes](https://github.com/BlueQuartzSoftware/simplnx/issues/1783).

Stage 04 uses the same complementary 89-point and 81-point tiles. Its generic HDF5 import deliberately selects four numeric datasets per tile from a known NX file; Read DREAM3D is simpler when the entire NX structure is needed. Tuple/component dimensions, XYZ ordering, A-then-B row ordering, and Micrometer units are explicit. The six-column CSV preserves integer source indices and feature IDs, physical coordinates, and inherited image quality. It exports every point, including tuple zero. No new acquisition, calibration, normalization, or duplicate-removal result is claimed.

Stage 04 also maps all 170 source points to explicit 0.5 micrometer cells and compares within-cell image-quality averages with Gaussian spreading over a 3 by 3 by 3 cell neighborhood. Original point arrays and the CSV remain unchanged. Contribution counts identify observed cells, cells estimated only by neighboring observations, and empty cells; interpolation does not add measurements or reconstruct a grain boundary. The companion explains the physical support and index-offset sigma conventions.


Stage 03 also compares 5,000 native surface samples with the 170 source voxel centers and approximate hulls at 0.25 and 0.5 micrometer pitches (156 and 43 representative points). A separate 0.5 micrometer grid labels source image cells by spatial bin without replacing grain IDs. The companion explains the distinction between measured-derived locations, simulated surface sampling and unconnected hull representatives, plus the current attribute-transfer and vertex-preflight limitations. These additions use the same packaged source data and preserve the original registration and interchange arrays.


Stage 02 now samples the complete shell back onto the source grid and exactly recovers its 170 cells. It compares the unique-surface-vertex mean with a reconstructed-volume centroid, then uses fixed-seed positioning bounds of 0.05 and 0.20 micrometers to illustrate correlated row/slice sampling effects. Correctly associated output grids retain 170 and 180 selected cells; the larger case changes 62 memberships without changing the mesh. The companion explains local labels, nominal storage positions, the metadata workaround and why triangle-shape outputs remain deferred. The original CSV columns remain intact and nominal voxel measurements are added.


Stage 03 now saves the individual challenge rotation and translation, combines them as T*R, and applies the composite once. A separate sequential cloud verifies the same intended motion; the reverse-order R*T matrix is retained for inspection and changes the translation by about 0.043631 micrometers. Original challenge, registration, sampling and scalar data remain unchanged. The companion describes the sixteen-scalar matrix layout and multiplication order.


Stage 01 also writes `SmallIN100_Grain2205_Nodes.txt` and `SmallIN100_Grain2205_Triangles.txt` beside its native checkpoint. The numbered space-delimited files contain 256 compact nodes and 508 triangles with zero-based IDs. Coordinates use micrometer numbers rounded to four decimal places; retain the native file for full precision, units and original ownership arrays. The text tables are not a complete solver model.
