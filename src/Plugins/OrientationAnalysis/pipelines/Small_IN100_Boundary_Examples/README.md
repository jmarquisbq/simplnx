# Small IN100 boundary and section example

This category contains one native pipeline for comparing cell-boundary exposure, grid-step boundary distance, centroid-box selection, voxel surface-to-volume ratio, and largest grid-aligned sections of reconstructed Small IN100 grains. The `.d3dpipeline` is executable authority; the matching Markdown explains interpretation, and its JSON-compatible YAML records the exact arguments. `FilterCoverage.json` lists the six core-filter targets. `PythonGeneration.yaml` describes a future generated-Python check; generated execution is not established by the native run.

## Run order and paths

Run these native pipelines from one writable working directory, in this order:

1. `Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline` imports the 117 public ANG sections and creates `Data/Output/Reconstruction/Small_IN100.h5ebsd`.
2. `Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline` creates `Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d` with final `FeatureIds`.
3. `Small_IN100_Examples/(02) Small IN100 Feature Measurements.d3dpipeline` creates `Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.dream3d`.
4. [(01) Small IN100 Boundary and Section Measurements](%2801%29%20Small%20IN100%20Boundary%20and%20Section%20Measurements.md) reads that checkpoint and writes `Data/Output/Small_IN100_Boundary_Examples/Measurements/SmallIN100_BoundarySections.dream3d`.

The `Data/...` paths resolve from the runner's current working directory. The [Small IN100 examples README](../Small_IN100_Examples/README.md#quick-start-117-section-3d-example) explains downloading and extracting [Small_IN100.tar.gz](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) so `Data/Small_IN100/Slice_1.ang` through `Slice_117.ang` are present. Its SHA-512 is `79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b`. No ANG sections or DREAM3D volume are bundled in this category.

From a PowerShell session, set the paths and run:

```powershell
$work = 'C:\path\to\writable\SmallIN100Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines' # Parent of Small_IN100_Processing and Small_IN100_Examples
Set-Location $work
New-Item -ItemType Directory -Force 'Data/Output/Reconstruction' | Out-Null
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Examples/Preparation' | Out-Null
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Examples/Measurements' | Out-Null
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Boundary_Examples/Measurements' | Out-Null
foreach ($relative in @(
    'Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline',
    'Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline',
    'Small_IN100_Examples/(02) Small IN100 Feature Measurements.d3dpipeline',
    'Small_IN100_Boundary_Examples/(01) Small IN100 Boundary and Section Measurements.d3dpipeline'
)) {
    $pipeline = Join-Path $examples $relative
    & $runner --execute $pipeline
    if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $pipeline (exit $LASTEXITCODE)" }
}
```

Create or copy the complete predecessor `Data/Output` tree before running only step 4. For the GUI, use **File > Open** and set absolute input/output paths if its working directory differs. The registered native pipeline passed. Its saved output contains 2,317 occupied positive features, 1,349 original interior features, and 1,027 centroid-box interior features. The [companion](%2801%29%20Small%20IN100%20Boundary%20and%20Section%20Measurements.md) explains the measurements and selection limits. OOC, generated Python, GUI, and exact paper-result reproduction remain unverified. A [confirmed surface-area defect](https://github.com/BlueQuartzSoftware/simplnx/issues/1780) affects X/Y-normal face weighting when X and Y spacing differ; this isotropic example is unaffected.

This example uses the full 189 × 201 × 117 measured-derived reconstruction on its isotropic 0.25 µm grid. The [Groeber and Jackson (2014) paper](https://doi.org/10.1186/2193-9772-3-5) provides reconstruction and feature-statistics context. The new descriptor comparison is illustrative; its numerical arrays are not claimed as a published result. The article is CC BY 2.0, while separate archive redistribution terms have not been established.
