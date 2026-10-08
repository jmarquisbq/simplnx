# Small IN100 geometry examples

These three examples turn a bounded, measured-derived EBSD label volume into a triangle interface mesh, compare constrained smoothing, and demonstrate a rigid transform followed by STL export. They use SimplnxCore geometry filters with the OrientationAnalysis Small IN100 preparation workflow. The category is flat: each numbered `.d3dpipeline` has a matching Markdown companion and JSON-compatible YAML sidecar beside it.

> **Validation warning:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

## Choose an example

| Example | Purpose | Required predecessor |
| --- | --- | --- |
| `(01) Small IN100 Surface Mesh` | Crop to 24 cubed cells, create unsmoothed interfaces, and calculate face geometry | Feature Preparation from `Small_IN100_Examples` |
| `(02) Small IN100 Surface Smoothing` | Create a second mesh from the same cropped labels with constrained relaxation | Geometry example 01 |
| `(03) Small IN100 Surface Transform and Export` | Copy the baseline, rotate and translate it, then export STL | Geometry example 01 |

Examples 02 and 03 are separate branches from 01. Transform/export does not use the smoothed mesh. After changing the preparation or crop, rerun 01 and regenerate both branches; after changing only smoothing, rerun 02. Each pipeline saves to its own output directory.

## Prepare data and run

Use one writable working folder throughout. Relative `Data/...` paths resolve from that folder for CLI execution. The full measured reconstruction requires all 117 ANG sections; this is not a single-section example.

1. Obtain the public [Small_IN100 archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) and extract `Slice_1.ang` through `Slice_117.ang` under `Data/Small_IN100`.
2. Run `../Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline`. It writes `Data/Output/Reconstruction/Small_IN100.h5ebsd`.
3. Run `../Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline`. It writes `Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d`.
4. Create the output directories below, then run geometry example 01.
5. Run 02 for the smoothing comparison, 03 for transform/export, or both. Read the corresponding companion before changing parameters.

```powershell
New-Item -ItemType Directory -Force -Path `
  'Data/Output/Small_IN100_Geometry_Examples/Surface', `
  'Data/Output/Small_IN100_Geometry_Examples/Smoothing', `
  'Data/Output/Small_IN100_Geometry_Examples/Transform'
```

Set `$nxrunner` to the matching application executable and `$suite` to this installed or source category. From the chosen working folder, run the geometry chain after preparation succeeds:

```powershell
$nxrunner = 'C:\path\to\nxrunner.exe'
$suite = 'C:\path\to\Small_IN100_Geometry_Examples'
foreach ($name in @(
  '(01) Small IN100 Surface Mesh.d3dpipeline',
  '(02) Small IN100 Surface Smoothing.d3dpipeline',
  '(03) Small IN100 Surface Transform and Export.d3dpipeline'
)) {
  & $nxrunner --execute (Join-Path $suite $name)
  if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $name (exit $LASTEXITCODE)" }
}
```

Keep the same working folder for every stage. In DREAM3D-NX, open a pipeline through File > Open or its prebuilt bookmark. If relative files do not resolve from the GUI launch folder, set absolute file input/output paths. Native GUI interaction remains a separate verification step.

## Outputs and contracts

All paths below begin with `Data/Output/Small_IN100_Geometry_Examples/`.

| File | Contents |
| --- | --- |
| `Surface/SmallIN100_SurfaceMesh.dream3d` | Cropped `DataContainer`, `SurfaceMesh`, face labels, node types, areas, and normals |
| `Smoothing/SmallIN100_SurfaceSmoothing.dream3d` | Same crop and baseline plus `SmoothedSurfaceMesh` with fresh measurements |
| `Transform/SmallIN100_SurfaceTransform.dream3d` | Same crop and baseline plus `SurfaceMesh_Transformed` and `SurfaceTransformMatrix` |
| `Transform/SmallIN100_SurfaceTransformed.stl` | One binary STL of all transformed triangles; no label arrays or explicit unit declaration |

The crop uses inclusive XYZ `[82,88,46]` through `[105,111,69]`. It preserves original `FeatureIds`, crops every source cell array, and does not renumber features. It starts from preparation rather than a whole-volume feature-measurement checkpoint. `ParentIds` are not interchangeable with `FeatureIds`.

SurfaceNets is the current recommended default mesher. Both mesh examples retain all bounding-box caps and attempt winding repair. The surface network includes internal interfaces and artificial crop caps; it is not a single exterior shell or a certified closed manifold. `FaceLabels=-1` denotes the exterior side, while zero remains a possible background label for other crops.

Smoothing uses 10 iterations, factor 0.5, and a 0.5 per-coordinate cell-space displacement limit. At 0.25 spacing this gives a 0.125 coordinate-unit bound per axis, not a 0.5 physical-unit or radial tolerance. The current code's local clamp governs this interpretation. The input image uses Micrometer units. SurfaceNets otherwise leaves the new mesh at default Meter metadata, so each meshing pipeline creates a temporary mesh, copies its vertices and triangles through `CreateGeometryFilter` with length-unit index 6, copies FaceLabels and NodeTypes, and deletes the temporary geometry. This preserves numerical coordinates and topology while explicitly recording Micrometer units. The transform branch also uses explicit Triangle/Copy/Micrometer construction from the baseline vertex and triangle arrays, because a whole-geometry deep copy resets units to Meter. Separate array copies retain FaceLabels and NodeTypes.

The transform is `x_new=10-y`, `y_new=x`, `z_new=z`, without centering. The newly constructed geometry has no copied areas or normals; after transforming, the pipeline creates `TransformedFaceAreas` and `TransformedFaceNormals`. No cell labels or crystallographic orientations are rotated.

## Scientific scope and provenance

[Groeber and Jackson (2014)](https://doi.org/10.1186/2193-9772-3-5) supply the workflow context: geometric microstructure representation and the 117-section Ni-based-superalloy reconstruction-and-meshing case. Publisher supplements include processed examples; the public archive's exact identity with those outputs has not been established. The article is CC BY 2.0. Separate raw-archive redistribution terms were not established. This category includes no raw or processed volume files.

The public archive source SHA-512 recorded by the predecessor suite is:

```text
79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b
```

[Frisken (2022)](https://jcgt.org/published/0011/01/03/paper.pdf) provides the SurfaceNets algorithm reference. Its 21-page paper explains shared multi-label interfaces and constrained relaxation; its synthetic and medical examples are not this alloy crop. The article is CC BY-ND 3.0, and its code supplement is MIT licensed. No paper figure is included. The current diagonal-area helper does not implement the paper's effective minimum-area diagonal choice, so these examples make no claim of that section 4.4 behavior or intersection-free surfaces.

All three examples have `illustrative` reproduction status. The crop, modern mesher, smoothing values, rigid transform, and STL export differ from the 2014 case. Successful preflight or execution alone does not raise that status.

## Companions, Python, and validation

The `.d3dpipeline` is executable authority. Markdown explains interpretation and adaptation; YAML records exact filter order, UUIDs, arguments, prerequisites, citations, and constraints. `FilterCoverage.json` records filter occurrences only, not scientific validation or complete plugin coverage.

`PythonGeneration.yaml` specifies temporary generation through `simplnx_utilities.generate_python_pipeline`. Regenerate from the pipeline after any change. Use the matching application's Python environment and the same data working folder. Report missing imports or conversion failures; do not silently hand-translate unsupported arguments. No persistent Python sidecar is installed.

On October 8, 2026, the three-pipeline Windows in-memory Release CTest chain passed in 20.38 seconds. Independent saved-data checks passed 108/108, covering every cropped source cell array, labels, mesh coordinates/connectivity, face measurements, explicit Micrometer metadata, smoothing bounds, the rigid transform, and every binary STL facet. The three figures were generated from those saved outputs and visually inspected. This scope does not include native GUI execution, OOC, generated-Python execution, downstream solver or printing behavior, or a proof of manifoldness or self-intersection freedom.

| Result | Checked values for the supplied crop |
| --- | --- |
| Baseline | 23 original positive regions; 6,479 vertices; 14,052 triangles; area 439.125 µm² |
| Smoothing | Same triangles and labels; mean displacement 0.0865 µm; maximum 0.2165 µm; area 346.630 µm² |
| Transform/export | Exact prescribed coordinate mapping; unchanged area; STL 14,052 facets and 702,684 bytes |

Area totals include internal interfaces and artificial caps. The smoothing result includes very small triangles, so smoother appearance is not a mesh-quality guarantee. All three published mesh variants explicitly record Micrometer units. The surface figures display only the original-ID pair 533/1719 (1,138 triangles) for clarity; exported files retain the full mesh. The source-image preview uses crop slice 12, centered at z = 14.625 µm. PNGs are data-derived scientific views, not screenshots of DREAM3D-NX or paper figures. Their source-output and image hashes are recorded in the YAML companions.
