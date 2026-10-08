# T12 reporting examples

These three examples continue the public T12 IF-steel EBSD orientation workflow. They turn the grain orientation spread from the [T12 orientation examples](../T12_Orientation_Examples/README.md) into a relative feature field, show the effect of broadcasting grain values to pixels, and export a compact feature table. The `.d3dpipeline` files are the executable authority. Each has a Markdown explanation and JSON-compatible YAML metadata.

> **Before adapting:** Check the paper-linked data's provenance, the map spacing and phase symmetry, the validity masks, and the segmentation and reporting rules. These pipelines illustrate reporting decisions for one map; they do not establish a material acceptance threshold or reproduce the paper's numerical results.

## Get the data and run

Follow the [orientation suite setup](../T12_Orientation_Examples/README.md) to obtain the public `T12-MAI-2010` archive and produce `Data/Output/T12_Orientation_Examples/Metrics/T12_MisorientationMetrics.dream3d`. Keep the archive's `PublicRelease.txt`. The archive release notice attributes permission for public release and DREAM.3D distribution to Francis Wagner; it does not state a standard license. Neither the raw scan nor the article is copied into this example directory.

Run from the same short, writable CLI working folder used for the orientation examples. Relative `Data/...` paths resolve from that folder. In the GUI, open each pipeline and set absolute file paths if the launch working directory differs.

```powershell
$work = 'C:\DREAM3D\T12Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines\T12_Reporting_Examples'
Set-Location $work
& $runner --execute (Join-Path $examples '(01) T12 Derived Reporting Fields.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Derived reporting fields failed; inspect runner output.' }
& $runner --execute (Join-Path $examples '(02) T12 Feature to Cell Mapping.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Feature mapping failed; inspect runner output.' }
& $runner --execute (Join-Path $examples '(03) T12 Compact Report.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Compact report failed; inspect runner output.' }
```

[(01) Derived Reporting Fields](%2801%29%20T12%20Derived%20Reporting%20Fields.md) reads the full metrics checkpoint, summarizes GOS over `ReportableFeatures` with equal grain weighting, and adds dimensionless `GOSZScore` only for that cohort. Check a positive, finite population standard deviation before interpreting z scores. Its output is `Derived/T12_ReportingFields.dream3d` under `Data/Output/T12_Reporting_Examples`.

[(02) Feature to Cell Mapping](%2802%29%20T12%20Feature%20to%20Cell%20Mapping.md) reads that output, broadcasts GOS, z score, and the reporting mask through `FeatureIds`, and summarizes GOS over accepted cells. Its `Mapping/T12_FeatureMaps.dream3d` result allows a pixel-weighted mean to be compared with the equal-grain mean from 01. It does not change source labels or calculate new per-pixel orientation differences.

[(03) Compact Report](%2803%29%20T12%20Compact%20Report.md) independently reads 01. It copies the feature Attribute Matrix to `Reporting/Cell Feature Data`, trims only the copy to six scalar fields, and writes `Report/T12_CompactReport.csv` and `Report/T12_CompactReport.dream3d`. The CSV skips background row 0 but keeps every positive feature ID, including rows with `ReportableFeatures=0`. Its stable header is `Feature_ID,EquivalentCircleDiameters,GOSZScore,GrainOrientationSpread,NumElements,PixelAreas,ReportableFeatures`. The [data import examples](../T12_Import_Examples/README.md) reuse its CSV or select only the `Reporting` group from the DREAM3D file.

The maps use one 2D slice with 0.5 micrometer X and Y spacing. GOS is in degrees, equivalent circle diameter in micrometers, and pixel area in square micrometers. `GOSZScore` is dimensionless and relative to the selected grain cohort. Broadcasting a feature value to cells repeats it; a cell-weighted mean counts larger grains more often. Keep the population and unit visible when comparing summaries or files.

[Allain-Bonasso et al. (2012)](https://doi.org/10.1016/j.msea.2012.03.068) motivates the grain-level orientation questions. Z scores, the broadcast maps, and the compact table are additional NX teaching choices. The examples omit paper-specific noise reduction, deformation-state assignment, matched maps, and independent materials validation.

## Reference results

The supplied map selects 1,443 grains. Their equal-grain mean GOS is **2.325059°**, with population deviation **1.311755°**. They occupy 1,036,035 pixels; pixel weighting raises the mean to **3.330604°**. The report CSV keeps all 3,059 positive feature IDs and seven columns, including the reporting flag. Each companion shows plots from these output files.

To regenerate after changing segmentation, spacing, or the reporting threshold, rerun the orientation preparation and metrics, then derived fields and the desired reporting branch. Recreate the compact report before either import example. `PythonGeneration.yaml` describes optional temporary Python conversion; `FilterCoverage.json` lists the filters used.

These examples passed Windows in-memory preflight/execution and independent output checks. GUI use, OOC, generated Python and full application installation remain unverified; scientific interpretation still requires review.
