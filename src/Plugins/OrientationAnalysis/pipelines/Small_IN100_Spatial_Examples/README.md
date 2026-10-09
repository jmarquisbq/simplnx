# Small IN100 spatial examples

These two pipelines connect grain location with local image-quality context. Example 01 compares inclusive voxel indices with physical outer-cell-face bounds. Example 02 compares a grain's processed Image Quality (IQ) distribution with statistics of every cell inside its rectangular box. The pipelines preserve the measured-derived image and final `FeatureIds`.

> **Validation warning:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

| Example | Main question | Predecessor |
| --- | --- | --- |
| `(01) Small IN100 Feature Bounds` | Where does each original feature lie in voxel and physical coordinates? | Feature Measurements from `Small_IN100_Examples` |
| `(02) Small IN100 Feature and Box Quality` | How does grain-member IQ differ from the IQ in its surrounding box? | Spatial example 01 |

## Prepare and run

Use one writable working folder for the complete chain. Relative `Data/...` file paths resolve from that folder when running the CLI. The input is the full 117-section reconstruction; a single ANG slice is insufficient.

1. Obtain the public [Small_IN100 archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz). Extract `Slice_1.ang` through `Slice_117.ang` into `Data/Small_IN100`.
2. Run `../Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline`, producing `Data/Output/Reconstruction/Small_IN100.h5ebsd`.
3. Run `../Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline`, producing `Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d`.
4. Run `../Small_IN100_Examples/(02) Small IN100 Feature Measurements.d3dpipeline`, producing `Data/Output/Small_IN100_Examples/Measurements/SmallIN100_FeatureMeasurements.dream3d`.
5. Create the two output folders and run spatial examples 01 and 02 in order.

```powershell
New-Item -ItemType Directory -Force -Path `
  'Data/Output/Small_IN100_Spatial_Examples/Bounds', `
  'Data/Output/Small_IN100_Spatial_Examples/Quality'

$nxrunner = 'C:\path\to\nxrunner.exe'
$suite = 'C:\path\to\Small_IN100_Spatial_Examples'
foreach ($name in @(
  '(01) Small IN100 Feature Bounds.d3dpipeline',
  '(02) Small IN100 Feature and Box Quality.d3dpipeline'
)) {
  & $nxrunner --execute (Join-Path $suite $name)
  if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $name (exit $LASTEXITCODE)" }
}
```

In DREAM3D-NX, open each pipeline through File > Open or the prebuilt category. If the GUI's working directory does not resolve the relative files, set absolute input and output paths. Regenerate both spatial stages after changing labels, geometry, or preparation. A change confined to IQ histogram bins requires rerunning only example 02.

## Outputs and population rules

Paths below begin with `Data/Output/Small_IN100_Spatial_Examples/`.

| Output | Contents |
| --- | --- |
| `Bounds/SmallIN100_FeatureBounds.dream3d` | Original data plus index bounds, raw physical bounds, occupancy mask, and stable box-query coordinates |
| `Quality/SmallIN100_FeatureAndBoxQuality.dream3d` | Bounds checkpoint plus per-feature IQ histograms and all-cell box statistics |

New feature arrays are in `DataContainer/Cell Feature Data`; histogram arrays are in `DataContainer/FeatureIQHistograms/"Image Quality" Histogram`. Rows keep the original `FeatureIds`, including gaps. Do not substitute twin-processing `ParentIds`.

`FeatureIndexBounds` uses inclusive minXYZ/maxXYZ voxel indices. `FeaturePhysicalBounds` uses outer cell faces in micrometers. The input origin has small nonzero offsets, which remain part of the coordinates. `BoxQueryBounds` adds half the supplied voxel spacing (0.125 µm) to all six coordinates, then sets background and unused rows to zero. This avoids edge-rounding losses in the box filter's floor-based conversion to a half-open voxel interval. The query array does not redefine physical feature extents. Adapt this offset per axis for anisotropic spacing, and verify the selected voxel intervals if changing geometry or numeric scale.

`OccupiedPositiveFeatures` is `NumElements > 0`. The measurements predecessor intentionally sets row 0 to zero even though background cells exist. Raw physical bounds include observed background, while raw integer corners skip it; absent positive rows have NaN physical bounds and UINT32_MAX index minima. Those raw values are retained, and excluded query boxes are empty. Use the occupancy mask and `BoxHasData` to interpret statistics.

IQ histograms use all cells and 16 shared bins over `[0,320)`, width 20. Row 0 remains available for background inspection but is excluded from grain reports. Empty rows have zero counts and zero ranges. Box statistics include other grains and background within each rectangle; overlapping boxes are not independent samples. Processed IQ inherits alignment and cleanup and is neither a raw detector measurement nor an accuracy estimate.

## Scientific scope and provenance

[Groeber and Jackson (2014)](https://doi.org/10.1186/2193-9772-3-5), *DREAM.3D: A Digital Representation Environment for the Analysis of Microstructure in 3D*, provides reconstruction and feature-statistics context (Figure 5; Tables 1–2). Publisher supplements provide a processed Ni-superalloy volume and grain statistics. The public archive's exact identity with those outputs is not established. These examples have **illustrative** reproduction status: bounds, IQ bins, box comparisons, and numerical query handling are explicit extensions, without a named published result to reproduce.

The article is CC BY 2.0. Separate raw-archive redistribution terms were not established. This source category contains pipelines, companions, and data-derived figures; no raw or processed volume is included. It reuses the predecessor archive and transformations without new datasets or private assets. The archive's recorded source SHA-512 is:

```text
79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b
```

The input is measured-derived: 117 measured ANG sections, reconstructed and cleaned by the preparation chain, followed by feature measurements. No synthetic image is substituted. New arrays retain geometry units and source label membership.

## Companion and validation scope

The `.d3dpipeline` is executable authority. Markdown explains choices and interpretation; JSON-compatible YAML records exact zero-based step order, UUIDs, parameter versions, parameters, input provenance, and constraints. `FilterCoverage.json` records occurrences in this suite, not scientific validation or a complete plugin inventory.

`PythonGeneration.yaml` defines temporary generation through `simplnx_utilities.generate_python_pipeline`; regenerate after changing a pipeline. Use the matching application modules and documented data working folder. Report unsupported conversion instead of silently translating by hand. No persistent Python sidecar is installed.

On October 9, 2026, the two-pipeline Windows in-memory Release CTest chain passed in 21.69 seconds. Independent saved-data checks matched every index/physical-bound row, occupancy and query array, histogram count/range/most-populated-bin value, and box statistic; all 27 source datasets and image geometry were preserved against the exact runtime predecessor. Float64 comparisons found a maximum mean difference of 0.001724 IQ units and a maximum population-standard-deviation difference of 0.000003968, consistent with float32 output and accumulation.

There are 2,335 feature rows: 2,317 occupied positive labels, 17 unused positive labels, and background row 0. Histogram totals cover all 4,444,713 cells, including 1,057,632 background cells, with no overflow. All intended voxel envelopes match the prepared query bounds; 2,313 boxes also contain other labels. The two figures use saved results for feature 533 and were visually inspected. Its 7,571 member cells occupy 22.4% of a 33,792-cell box containing 46 labels.

Native GUI use, OOC storage, and generated-Python execution remain unverified. These checks establish this example's data contracts, not an accuracy, acceptance, or exact paper-reproduction claim.
