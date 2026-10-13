# T12 orientation examples

These two examples answer distinct questions using one measured steel EBSD map. Read each guide's **Before adapting** notice before changing inputs.

| Order | Task |
|---|---|
| [(01) T12 Orientation Case Study](%2801%29%20T12%20Orientation%20Case%20Study.md) | Prepare the map, compare local and grain-reference orientation variation, and test KAM neighborhood choices in one combined pipeline. |
| [(02) T12 Orientation Interchange](%2802%29%20T12%20Orientation%20Interchange.md) | Read the preparation checkpoint; preserve source measurements while changing Euler units and quaternion order, inspecting conjugation, and exporting indexed angle records. |

## Get the data and run

Download the public [T12-MAI-2010 archive](https://www.dream3d.io/Data_Archive/T12-MAI-2010.tar.gz), extract it into a writable working folder's `Data/` directory, and keep `PublicRelease.txt`. The required input is `Data/T12-MAI-2010/fw-ar-IF1-aptr12-corr.ctf`; the second map is not required. Installed example data may already provide it. Use a short Windows working path because the writer creates a temporary subdirectory.

```powershell
$work = 'C:\DREAM3D\T12Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines\T12_Orientation_Examples'
New-Item -ItemType Directory -Force -Path "$work\Data" | Out-Null
tar -xzf 'C:\path\to\T12-MAI-2010.tar.gz' -C "$work\Data"
if ($LASTEXITCODE -ne 0) { throw 'Data extraction failed.' }
Set-Location $work
& $runner --execute (Join-Path $examples '(01) T12 Orientation Case Study.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Case study failed; inspect the runner output.' }
& $runner --execute (Join-Path $examples '(02) T12 Orientation Interchange.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Interchange failed; inspect the runner output.' }
```

Relative paths resolve from the CLI working folder. In the GUI, open the pipeline and set absolute file paths if the launch location does not resolve `Data/...`. Example (01) writes preparation, metrics, and complete neighborhood checkpoints under `Data/Output/T12_Orientation_Examples/`, retaining the paths used by the separate reporting and import examples. Example (02) reads `Preparation/T12_Orientations.dream3d` and writes `Interchange/T12_OrientationInterchange.dream3d` plus `Interchange/T12_IndexedAngles_Phase_1.txt`. The configured text base name gains the phase suffix. Rerun (01) before its dependents after a processing change.

## Scientific basis and scope

[Allain-Bonasso et al. (2012)](https://doi.org/10.1016/j.msea.2012.03.068) motivates the **illustrative** single-map case study. It applies the default Oxford/HKL CTF sample-frame correction, preserves measured orientations, and explicitly compares NX neighborhood definitions. It does not reproduce the paper's noise reduction, four-neighbor KAM, grain tracking, or deformation results. Interchange is a separate teaching extension that preserves this checkpoint; it computes no ODF and claims no article result. The archive's release notice attributes public distribution permission to Francis Wagner and names no standard license. Raw data and the article are not included here.

The `.d3dpipeline` is executable authority; its Markdown and JSON-compatible YAML are human and structured companions. `FilterCoverage.json` lists every pipeline step. `PythonGeneration.yaml` describes temporary Python generation, which remains unverified. The older `EBSD_File_Processing/aptr12_Analysis.d3dpipeline` is preserved for comparison.

Execution and numerical verification scope is recorded in the companion YAML. GUI execution, out-of-core storage, generated Python execution, and materials interpretation remain unverified.
