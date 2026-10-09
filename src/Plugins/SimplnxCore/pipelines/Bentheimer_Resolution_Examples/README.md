# Bentheimer resolution examples

These three pipelines import two registered Bentheimer sandstone CT scans at their native voxel sizes, compare them on one grid, and measure a matching interior region. They use the full released Core 1 / Subvolume 1 author-normalized cubes from [Zenodo](https://doi.org/10.5281/zenodo.5542624). Each pipeline preserves its inputs in a reusable checkpoint.

> **Before adapting:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

| Example | Main question | Prerequisite |
| --- | --- | --- |
| `(01) Bentheimer Native Volume Preparation` | How do I import headerless CT data while preserving physical size? | Two verified raw files |
| `(02) Bentheimer Common Grid Comparison` | Where do the registered scans differ on a common grid? | Example 01 |
| `(03) Bentheimer Interior Region Comparison` | How do I measure the same physical interior at both voxel sizes? | Example 02 |

## Acquire the data and run

Choose one writable working folder for the whole chain. CLI relative file paths resolve from that folder. Download the following **author-normalized** files from the [dataset record](https://zenodo.org/records/5542624) into `Data/Bentheimer_Resolution_Examples`, preserving their filenames. No extraction, conversion, or new archive publication is required.

| Filename | Exact byte length | Native grid |
| --- | ---: | --- |
| `Core1_Subvol1_18micron_75cube_16bit_LE_normalised.raw` | 843,750 | 75³ cells, 18 µm spacing |
| `Core1_Subvol1_6micron_225cube_16bit_LE_normalised.raw` | 22,781,250 | 225³ cells, 6 µm spacing |

[DataSources.json](DataSources.json) contains direct download URLs and the complete SHA-512 hashes. Verify **both** length and checksum before running; similarly named unnormalized files are different inputs. These PowerShell commands download missing files, preserve existing files, and reject a mismatch:

```powershell
$suite = 'C:\path\to\Bentheimer_Resolution_Examples'
$dataFolder = 'Data/Bentheimer_Resolution_Examples'
New-Item -ItemType Directory -Force -Path $dataFolder | Out-Null
$manifest = Get-Content -Raw -LiteralPath (Join-Path $suite 'DataSources.json') | ConvertFrom-Json
foreach ($asset in $manifest.files) {
  $destination = Join-Path $dataFolder $asset.filename
  if (-not (Test-Path -LiteralPath $destination)) {
    Invoke-WebRequest -Uri $asset.url -OutFile $destination
  }
  if ((Get-Item -LiteralPath $destination).Length -ne $asset.bytes -or
      (Get-FileHash -LiteralPath $destination -Algorithm SHA512).Hash.ToLowerInvariant() -ne $asset.sha512) {
    throw "Input size or SHA-512 mismatch: $destination"
  }
}

New-Item -ItemType Directory -Force -Path `
  'Data/Output/Bentheimer_Resolution_Examples/Preparation', `
  'Data/Output/Bentheimer_Resolution_Examples/Comparison', `
  'Data/Output/Bentheimer_Resolution_Examples/Regions' | Out-Null

$nxrunner = 'C:\path\to\nxrunner.exe'
foreach ($name in @(
  '(01) Bentheimer Native Volume Preparation.d3dpipeline',
  '(02) Bentheimer Common Grid Comparison.d3dpipeline',
  '(03) Bentheimer Interior Region Comparison.d3dpipeline'
)) {
  & $nxrunner --execute (Join-Path $suite $name)
  if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $name (exit $LASTEXITCODE)" }
}
```

In DREAM3D-NX, open each pipeline through File > Open or the prebuilt category. If relative paths do not resolve from the GUI's working directory, set absolute file input/output paths. Re-run all three examples after changing source data or import geometry; re-run examples 02 and 03 after changing scaling or mapping; re-run example 03 after changing the local frame or ROI.

## Outputs and interpretation

Paths below begin with `Data/Output/Bentheimer_Resolution_Examples/`.

| Checkpoint | Contents |
| --- | --- |
| `Preparation/ImportedVolumes.dream3d` | Both native geometries and original UInt16 intensities |
| `Comparison/CommonGridComparison.dream3d` | Native and mapped Float32 intensities, absolute difference, whole-volume summaries |
| `Regions/InteriorRegionComparison.dream3d` | Centered geometries, native cell-center coordinates, full-cell ROI masks, masked summaries |

The native geometries are `BentheimerVLR18um` and `BentheimerLR6um`; their scalar source arrays are `CellData/AuthorNormalizedIntensity`. Both cover a 1,350 µm cube. The initial lower corner is zero on all axes. Example 02 computes `ScaledIntensity = AuthorNormalizedIntensity / 20000` as Float32, preserving values above one. The fixed divisor follows the paper's display range; this is not an independent min-max normalization or probability conversion.

`BentheimerVLRMapped6um` repeats each VLR voxel three times per axis onto the LR grid while retaining the original. It adds no measured spatial detail. `BentheimerLR6um/CellData/AbsoluteScaledIntensityDifference` stores absolute differences; `BentheimerComparisonStatistics` contains `CellCount`, `Minimum`, `Maximum`, and `Mean`. A discrepancy can reflect noise, sampling, partial-volume effects, reconstruction, and residual registration. It is not an error against known truth.

Example 03 moves the lower corners of all three geometries to [−675,−675,−675] µm without changing spacing or intensity values. Native `CoordinatesMicrometers` arrays hold XYZ cell centers. `InteriorROI` selects complete cells inside [−405,405] µm on every axis. Counts of 91,125 VLR cells and 2,460,375 LR cells represent the same 531,441,000 µm³ volume. Native masked intensity summaries are in each geometry's `InteriorIntensityStatistics`; LR masked discrepancy summaries are in `BentheimerLR6um/InteriorDifferenceStatistics`.

## Scientific scope and provenance

[Jackson et al. (2022)](https://doi.org/10.1103/PhysRevApplied.17.054046), *Deep Learning of Multiresolution X-Ray Micro-Computed-Tomography Images for Multiscale Modeling*, provides the registered multiresolution imaging context. The full methods review used the versioned [arXiv v2](https://arxiv.org/html/2111.01270v2) from January 18, 2022 and its supplement. The final journal citation is *Physical Review Applied* **17**, 054046, published May 27, 2022. No paper figure is redistributed.

The data are measured-derived: the authors reconstructed, mutually registered, and normalized independent CT acquisitions. Their normalization procedure is not rerun here. The full released cubes are used without a new crop; source bytes remain unchanged. The 6 µm Core 1 / Subvolume 1 data served the paper's training workflow. The 18 µm data were shared for further work and were not used in its reported analysis.

All three examples have **illustrative** reproduction status. Nearest-neighbor 18-to-6 µm mapping is a transparent comparison baseline; it does not reproduce the paper's 6-to-2 µm EDSR or cubic interpolation. The local origin and 810 µm interior cube are example choices, not scanner coordinates, the paper's 600 µm display crop, or its representative elementary volume. No pore segmentation, porosity, permeability, or acceptance claim is made.

The data are CC BY 4.0. [SourceDataLicense.md](SourceDataLicense.md) provides attribution and transformation details, while [DataSources.json](DataSources.json) records file-level provenance. Raw and processed volume files are not committed in this source category. The three PNGs are newly rendered from saved pipeline outputs and carry the same source attribution.

## Companion and verification scope

The `.d3dpipeline` is executable authority. Markdown explains each task; JSON-compatible YAML records zero-based filter order, UUIDs, argument versions, source provenance, and constraints. `FilterCoverage.json` records occurrences, not scientific validation or complete plugin coverage. `PythonGeneration.yaml` specifies temporary conversion through `simplnx_utilities.generate_python_pipeline`; report missing bindings or unsupported arguments instead of silently writing a replacement implementation.

On October 9, 2026, all three pipelines passed preflight and execution in the Windows in-memory Release CTest chain in 2.01 seconds. The test used locally cached inputs with verified lengths and checksums. An independent checker passed 366 checks across the three saved checkpoints: original intensity arrays were preserved; mapped and scaled arrays, absolute differences, cell-center coordinates, and full-cell ROI masks matched independent typed oracles exactly; saved scalar statistics agreed within two Float32 ULPs. Physical dimensions, origins, spacing, micrometer units, scalar/component types, and tuple counts were also checked. Generated coordinate and mask arrays store flattened X-fastest cell tuples, with three and one components respectively.

The saved whole-volume mean absolute difference is 0.0478134714. The native VLR and LR ROI scaled-intensity means are 0.5362334251 and 0.5453459024; the ROI mean absolute difference is 0.0473593697. Values above one were retained: one native VLR voxel and 718 native LR voxels. These are fixture-specific discrepancy and intensity summaries, not material-property targets. The three PNGs were rendered from these saved outputs and inspected for physical axes, display limits, labels, and layout.

OOC, generated Python execution, native GUI operation, full installation, and exact paper-result reproduction remain unverified. The test wrapper's missing-cache HTTP download branch was not exercised by this cached-input run. Scientific limits remain unchanged by the successful computational checks.
