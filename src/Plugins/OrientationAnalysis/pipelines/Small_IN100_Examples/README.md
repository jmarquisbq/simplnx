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
foreach ($name in @('Preparation', 'Measurements', 'SurfaceSelection', 'MinimumSize', 'CombinedSelection')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $work "Data\Output\Small_IN100_Examples\$name") | Out-Null
}
foreach ($relative in @(
    'Small_IN100_Processing\(01) Small IN100 Archive.d3dpipeline',
    'Small_IN100_Examples\(01) Small IN100 Feature Preparation.d3dpipeline',
    'Small_IN100_Examples\(02) Small IN100 Feature Measurements.d3dpipeline',
    'Small_IN100_Examples\(03) Small IN100 Surface Selection Comparison.d3dpipeline',
    'Small_IN100_Examples\(04) Small IN100 Minimum Size Comparison.d3dpipeline',
    'Small_IN100_Examples\(05) Small IN100 Combined Selection.d3dpipeline'
)) {
    $pipeline = Join-Path $examples $relative
    & $runner --execute $pipeline
    if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $pipeline (exit $LASTEXITCODE)" }
}
```

Check that all 117 slices are present before running. Stop if any stage fails. The archive, preparation, and measurement stages write checkpoints under `Data/Output/`. The three selection branches each read the measurement checkpoint directly; none reads another branch's output. They write CSV and DREAM3D files under `Data/Output/Small_IN100_Examples/SurfaceSelection/`, `MinimumSize/`, and `CombinedSelection/`, respectively. CLI relative `Data/...` paths resolve from the chosen working folder; this was checked for the baseline chain and each selection branch in isolated Windows in-core Release runs.

For the GUI, use **File > Open** on each pipeline in the same order. GUI working folders vary by launch and installation layout. If relative `Data/...` paths do not resolve, set the pipeline's file input and output parameters to absolute paths in your chosen data/output folder before running. Native GUI execution of this bundle has not been verified. Each pipeline has a matching Markdown explanation and a JSON-compatible YAML sidecar. The pipeline is the executable authority. This category is flat so the installed runtime category has no extra bookmark folders.

The public [Small_IN100 archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) contains 117 measured ANG sections. The OrientationAnalysis CMake declaration uses SHA-512 `79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b`. The [official tutorial](https://dream3d.bluequartz.net/Help/2_Tutorials/EBSDReconstruction/) attributes the data to M. Uchic and colleagues at AFRL. No raw or processed volume files are included; the preview PNGs are derived from checked measurement and comparison outputs. The [Groeber and Jackson paper](https://doi.org/10.1186/2193-9772-3-5) supplies scientific context; the examples are **illustrative**, not exact paper reproductions. Raw-source identity with the publisher's processed supplement has not been proven. Confirm separate archive redistribution terms before repackaging the data.

The preparation copy changes only `EBSDSegmentFeatures.is_periodic` from true to false as a scientific parameter and moves the final output path. The original reconstruction remains unchanged. Measurements use cleaned `FeatureIds`, not twin `ParentIds`. The selection branches retain those IDs and positive CSV rows. Each writes a `ValidFeatures` mask from `NumElements > 0`, a selected mask, separate summaries for all valid and selected equivalent diameters, and matched 20-bin histograms over `[0, 10)` µm. The [surface comparison](%2803%29%20Small%20IN100%20Surface%20Selection%20Comparison.md) selects occupied features without detected image-box or face-adjacent background contact. The [minimum-size comparison](%2804%29%20Small%20IN100%20Minimum%20Size%20Comparison.md) selects occupied features with equivalent diameter at least 2.0 µm. The [combined selection](%2805%29%20Small%20IN100%20Combined%20Selection.md) applies both rules with an explicit AND condition. The cutoff is illustrative. Review each companion's assumptions before interpreting a result.

In the checked measurement checkpoint, background `NumElements[0]=0` and all 2,317 occupied diameters lie in `[0, 10)` µm. The surface rule retained 1,349 valid features and excluded 968. The 2.0 µm reporting cutoff retained 1,526 and excluded 791. Their combined intersection retained 853 valid features. All three branches passed isolated preflight and execution; independent checks matched their saved masks, summary statistics, common-bin histograms, CSV values, and unchanged original labels and measurements. Each selection companion gives the observed diameter summaries and a data-derived preview.

The registered CTest smoke test passed for the archive → preparation → measurement → surface, size, and combined selection sequence on a Windows in-core Release snapshot (1/1 in 18.10 seconds). The combined branch passed isolated preflight/execution and independent output checks, including a validation-only empty selection. Separate checks verified the baseline's counts, volumes, diameters, sampled centroids, and CSV values. See the measurement companion for the Windows `-nan(ind)` token produced for unused rows. The expanded 22-file source suite matched both runtime and staged plugin-install copies byte for byte, with no subdirectories. OOC, native GUI behavior, full application installation, and human interpretation review remain pending. `FilterCoverage.json` describes only the selected pilot filters. `PythonGeneration.yaml` is a generation specification; Python generation and execution are **not verified**.
