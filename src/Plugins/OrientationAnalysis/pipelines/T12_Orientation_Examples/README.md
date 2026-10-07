# T12 orientation examples

These examples ask how crystal orientation varies within a measured steel microstructure. They use a public 2D EBSD map and preserve its measured orientations so processing choices can be compared explicitly. Review each companion's **Before adapting** notice and **Data flow and choices** table before changing inputs.

## Get the data and run

Download the public [T12-MAI-2010 archive](https://www.dream3d.io/Data_Archive/T12-MAI-2010.tar.gz) and extract it into a writable working folder's `Data/` directory. Keep the archive's `PublicRelease.txt`. The preparation input must resolve to:

`Data/T12-MAI-2010/fw-ar-IF1-aptr12-corr.ctf`

If installed example data already provides this file, use it. Keep the working folder path short on Windows: the DREAM3D writer creates a temporary subdirectory, which can exceed a native library's path limit in deeply nested folders.

```powershell
$work = 'C:\DREAM3D\T12Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines\T12_Orientation_Examples'
New-Item -ItemType Directory -Force -Path "$work\Data" | Out-Null
tar -xzf 'C:\path\to\T12-MAI-2010.tar.gz' -C "$work\Data"
if ($LASTEXITCODE -ne 0) { throw 'Data extraction failed.' }
Set-Location $work
& $runner --execute (Join-Path $examples '(01) T12 Orientation Preparation.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Preparation failed; inspect the runner output.' }
& $runner --execute (Join-Path $examples '(02) T12 Local and Grain Reference Misorientation.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Misorientation calculation failed; inspect the runner output.' }
& $runner --execute (Join-Path $examples '(03) T12 KAM Neighborhood Comparison.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Neighborhood comparison failed; inspect the runner output.' }
```

Relative input/output paths resolve from the CLI working folder. For the GUI, open the pipeline and set absolute file paths if your launch location does not resolve `Data/...` correctly.

## Workflow and interpretation

[(01) Orientation Preparation](%2801%29%20T12%20Orientation%20Preparation.md) creates `Data/Output/T12_Orientation_Examples/Preparation/T12_Orientations.dream3d`. It saves a valid-pixel mask, grain labels, pixel counts and areas, a grain-reporting mask, and average orientations. It keeps rejected measurements and small grains in the saved data, with explicit masks for interpreting them.

[(02) Local and Grain Reference Misorientation](%2802%29%20T12%20Local%20and%20Grain%20Reference%20Misorientation.md) reads that checkpoint. It compares each pixel with nearby orientations in its grain and with its grain-average orientation, then saves pixel maps, grain spreads, clearly separated pixel/grain summaries, and a feature CSV under `Metrics/`. The summaries distinguish equal pixel weighting from equal grain weighting.

[(03) KAM Neighborhood Comparison](%2803%29%20T12%20KAM%20Neighborhood%20Comparison.md) compares two window radii within grains, then permits comparisons across grains in the same phase at the original radius. Results are saved under `Neighborhoods/`. **02 and 03 independently read 01**; 03 does not require 02. All comparisons retain the same prepared labels and orientations.

The source is one BCC iron phase sampled at 0.5 µm in X and Y. The second map in the archive is not required. Do not match pixels or grain IDs between the files from their names alone.

## Scientific basis and data terms

[Allain-Bonasso et al. (2012)](https://doi.org/10.1016/j.msea.2012.03.068), Sections 2.2–2.4, motivate the grain definition and orientation measures. The examples are **illustrative**: they omit the paper's noise reduction and multi-map grain matching and do not reproduce its deformation-dependent results. Its four-neighbor KAM definition differs from NX's rectangular kernel, which includes the center pixel. Orientation differences alone do not provide a calibrated strain or dislocation density.

The archive's release notice identifies the paper and attributes permission to Francis Wagner for public release and distribution with DREAM.3D. It does not name a standard license. Raw data and the article are not copied into this pipeline category.

Each pipeline has a Markdown explanation and JSON-compatible YAML metadata. The `.d3dpipeline` is the executable authority. `FilterCoverage.json` lists the selected filters. `PythonGeneration.yaml` describes generating temporary Python; it is not evidence of Python execution.

## Execution scope

Reference results were checked with the Windows command-line runner and in-memory storage. GUI execution, out-of-core storage and generated Python execution have not been checked for this suite. Materials interpretation remains pending expert review.
