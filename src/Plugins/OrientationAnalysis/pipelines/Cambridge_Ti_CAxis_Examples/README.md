# Cambridge titanium c-axis examples

These three linked, flat OrientationAnalysis examples use the original alpha-Ti Oxford EBSD map associated with [Krakow et al. (2017)](https://doi.org/10.1098/rspa.2017.0274). Each `.d3dpipeline` is executable authority; the matching `.md` explains choices and interpretation, the `.yaml` supplies structured context, and the three PNGs are rendered from the final native Release outputs. The suite is illustrative and does not reproduce the paper's full-misorientation twin classification.

## Before adapting

The original [Cambridge companion dataset](https://doi.org/10.17863/CAM.8815) is CC BY 4.0. [Download its original `upload_scripts_apollo.zip` archive](https://api.repository.cam.ac.uk/server/api/core/bitstreams/24eaae5b-5f74-405e-82e5-449f89ee570c/content). Credit Robert Krakow and coauthors and the Cambridge data repository when reusing its source or derived work. `DataSources.json` records the exact public archive URL, archive MD5/SHA-512 and byte count, and the two source-file SHA-512 hashes and byte counts. The CPR/CRC files are not bundled with these examples. Extract `upload_scripts_apollo/case_study_2_titanium_data.cpr` and `.crc` from `upload_scripts_apollo.zip` into `Data/CambridgeTitanium/` under your chosen writable working directory. Keep the pair alongside each other and verify hashes before running.

## Run order

| Stage | Purpose | Input | Checkpoint |
| --- | --- | --- | --- |
| [(01) Import and C-Axis Directions](%2801%29%20Cambridge%20Ti%20Import%20and%20C-Axis%20Directions.md) | Mask measured quality and project c axes | Original CPR/CRC | `Data/Output/Cambridge_Ti_CAxis_Examples/Prepared/CambridgeTi_Prepared.dream3d` |
| [(02) Grain C-Axis Metrics](%2802%29%20Cambridge%20Ti%20Grain%20C-Axis%20Metrics.md) | Segment full-orientation grains, calculate c-axis spread and neighbor angles | Stage 01 checkpoint | `Data/Output/Cambridge_Ti_CAxis_Examples/GrainMetrics/CambridgeTi_GrainMetrics.dream3d` |
| [(03) C-Axis Grouping Comparison](%2803%29%20Cambridge%20Ti%20C-Axis%20Grouping%20Comparison.md) | Compare direct cell c-axis regions with grouped grains | Stage 02 checkpoint | `Data/Output/Cambridge_Ti_CAxis_Examples/Grouping/CambridgeTi_GroupingComparison.dream3d` |

Run all three from the **same** working directory so relative `Data/...` paths resolve. Create output directories before calling the runner. With a matching DREAM3D-NX `nxrunner.exe` and the pipeline suite available:

```powershell
$work = 'C:\path\to\writable\TitaniumWork'
$runner = 'C:\path\to\nxrunner.exe'
$suite = 'C:\path\to\OrientationAnalysis\pipelines\Cambridge_Ti_CAxis_Examples'
Set-Location $work
New-Item -ItemType Directory -Force 'Data/Output/Cambridge_Ti_CAxis_Examples/Prepared', 'Data/Output/Cambridge_Ti_CAxis_Examples/GrainMetrics', 'Data/Output/Cambridge_Ti_CAxis_Examples/Grouping' | Out-Null
foreach ($stem in @('(01) Cambridge Ti Import and C-Axis Directions', '(02) Cambridge Ti Grain C-Axis Metrics', '(03) Cambridge Ti C-Axis Grouping Comparison')) {
  & $runner --execute (Join-Path $suite ($stem + '.d3dpipeline'))
  if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $stem (exit $LASTEXITCODE)" }
}
```

The imported map has 193,167 cells at 0.25 µm XY spacing; the explicit `Error==0 AND Bands>0 AND Phases==1` gate accepts 164,274. Native stage 02 found 1,285 populated full-orientation grains. Its `Areas > 1 µm²` reporting cohort contains 70 grains and 98.595% of indexed pixels; all 1,285 remain in the analysis. Native stage 03 found 1,272 cell c-axis regions and 1,269 populated grouped-grain parents. The two approaches agree closely here, with only three differing valid-neighbor edges; this result does not make their methods generally equivalent. The boundary preview uses a common c-axis backdrop and circles one difference in a tight zoom. Unit z spacing represents the single measured slice in software, so size and neighborhood results are **2D areas and area fractions**. Sample-Y correction changes the NX display convention relative to paper figure 10; do not transfer paper left/right labels.

The author `section4b.m` uses MTEX to classify full three-dimensional boundary misorientation clusters and twin relationships. This suite instead illustrates validity-aware c-axis direction, within-grain spread, and two c-axis grouping choices. It cannot identify twins, strain, stress, 3D connectivity, or macrozones. `edax_hexagonal_alignment=false` is deliberate, and the workflows do not add a compensating crystal/Euler rotation.

The registered Windows in-core Release CTest passed 1/1, fetching and hashing the public archive, extracting and hashing the two original files, and preflighting and executing all three final pipelines. An independent numerical/structural checker passed 69/69 assertions against the saved Release outputs, including all three partitions, c-axis spread, area conservation, neighborhood fractions, and the exact `ReportingGrains = Areas > 1 µm²` mask. Sixteen scientific arrays repeated exactly against the earlier run with the same Windows runtime and seed 5489. The neighbor-angle oracle consumes native `AvgQuats` and does not validate its averaging algorithm. Generated Python, OOC, GUI, and installed application execution remain unverified. `PythonGeneration.yaml` defines temporary conversion from the authoritative JSON when requested; no handwritten Python sidecar is included.


Preparation stage 01 also saves native direction RGB, sample-X inclination in degrees, Viridis preset RGB and monochrome display arrays. Quantitative inclination retains NaNs at excluded pixels; a separate finite copy supports color normalization, and all display layers retain the same validity mask. The companion names the preset and explains why generic vector colors, scalar colors and gray levels carry different information. Each grayscale call selects one array to avoid existing issue #1759. The original frame correction, hexagonal-alignment choice, grain metrics and grouping outputs remain unchanged.
