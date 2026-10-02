# Small IN100 feature measurement examples

## Quick start: 117-section 3D example

Choose a writable working folder. Its `Data/Small_IN100/` folder must contain `Slice_1.ang` through `Slice_117.ang`. If all 117 sections are already there from installed example data, use them. Otherwise download the public [Small_IN100.tar.gz](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) and extract it into the working folder's `Data/` folder; the archive contains a `Small_IN100/` folder. No ANG files are shipped in this category. Set `$runner` to the full path of `nxrunner.exe` and `$examples` to the folder that contains both `Small_IN100_Processing/` and `Small_IN100_Examples/`. In a Windows PowerShell session:

```powershell
$ErrorActionPreference = 'Stop'
$work = 'C:\path\to\writable\SmallIN100Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines' # Common parent of both Small_IN100 folders
New-Item -ItemType Directory -Force -Path (Join-Path $work 'Data') | Out-Null
$sections = @(1..117 | ForEach-Object { Join-Path $work "Data\Small_IN100\Slice_$($_).ang" })
if (@($sections | Where-Object { -not (Test-Path $_ -PathType Leaf) }).Count -gt 0) {
    tar -xzf 'C:\path\to\Small_IN100.tar.gz' -C (Join-Path $work 'Data')
    if ($LASTEXITCODE -ne 0) { throw "Archive extraction failed: $LASTEXITCODE" }
}
if (@($sections | Where-Object { -not (Test-Path $_ -PathType Leaf) }).Count -gt 0) { throw 'One or more of the 117 ANG sections are missing.' }
Set-Location $work
foreach ($relative in @(
    'Small_IN100_Processing\(01) Small IN100 Archive.d3dpipeline',
    'Small_IN100_Examples\(01) Small IN100 Feature Preparation.d3dpipeline',
    'Small_IN100_Examples\(02) Small IN100 Feature Measurements.d3dpipeline'
)) {
    $pipeline = Join-Path $examples $relative
    & $runner --execute $pipeline
    if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $pipeline (exit $LASTEXITCODE)" }
}
```

Check that all 117 slices are present before running. Stop if any stage fails; the next stage needs its output. The stages write `Data/Output/Reconstruction/Small_IN100.h5ebsd`, `Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d`, and the final CSV and DREAM3D under `Data/Output/Small_IN100_Examples/Measurements/`. CLI relative `Data/...` paths resolve from the chosen working folder; this was checked in the isolated CTest run.

For the GUI, use **File > Open** on each pipeline in the same order. GUI working folders vary by launch and installation layout. If relative `Data/...` paths do not resolve, set the pipeline's file input and output parameters to absolute paths in your chosen data/output folder before running. Native GUI execution of this bundle has not been verified. Each pipeline has a matching Markdown explanation and a JSON-compatible YAML sidecar. The pipeline is the executable authority. This category is flat so the installed runtime category has no extra bookmark folders.

The public [Small_IN100 archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) contains 117 measured ANG sections. The OrientationAnalysis CMake declaration uses SHA-512 `79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b`. The [official tutorial](https://dream3d.bluequartz.net/Help/2_Tutorials/EBSDReconstruction/) attributes the data to M. Uchic and colleagues at AFRL. No raw or processed volume files are included; the preview PNG is derived from the checked baseline output. The [Groeber and Jackson paper](https://doi.org/10.1186/2193-9772-3-5) supplies scientific context; the examples are **illustrative**, not exact paper reproductions. Raw-source identity with the publisher's processed supplement has not been proven. Confirm separate archive redistribution terms before repackaging the data.

The preparation copy changes only `EBSDSegmentFeatures.is_periodic` from true to false as a scientific parameter and moves the final output path. The original reconstruction remains unchanged. Measurements use cleaned `FeatureIds`, not twin `ParentIds`. For summaries, remove background row zero and positive-ID rows with `NumElements=0`; review surface-truncated features separately. See the measurement companion for equations, result inspection, and adaptation checks.

The exact archive → preparation → measurement chain passed six preflight/execution checks on a Windows in-core Release snapshot. Six independent arithmetic checks passed, and the CSV's 16 numeric columns matched the saved DREAM3D arrays. See the measurement companion for the data-derived preview and the Windows `-nan(ind)` token produced for unused rows. The runtime copy and staged plugin install each matched all ten source files byte for byte, with no subdirectories. OOC, native GUI behavior, full application installation, and human interpretation review remain pending. `FilterCoverage.json` describes only the selected pilot filters. `PythonGeneration.yaml` is a generation specification; Python generation and execution are **not verified**.
