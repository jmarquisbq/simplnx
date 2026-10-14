# Small IN100 boundary distributions

One pipeline compares a binned Sigma3 section, a metric-based Sigma3 section, and a boundary-plane distribution on the same prepared network. Read [(01) Small IN100 Boundary Distribution Comparison](%2801%29%20Small%20IN100%20Boundary%20Distribution%20Comparison.md) for the motivations, native preview, outputs, and limitations. The executable JSON is authoritative; its YAML records exact arguments and structured context.

## Get the input and run

Download the public [compute_gbcd_metric_based.tar.gz validation archive](https://github.com/BlueQuartzSoftware/simplnx/releases/download/Data_Archive/compute_gbcd_metric_based.tar.gz), **24,994,061 bytes**. Extract only `compute_gbcd_metric_based/compute_gbcd_metric_based.dream3d` and place it at:

```text
Data/Small_IN100_Distribution_Examples/compute_gbcd_metric_based.dream3d
```

Keep this original 58,868,934-byte file unchanged. `DataSources.json` records archive and member hashes. This prepared mesh is **not part of the normal example-data package**; its future package inclusion is pending review. It is also not the preparation checkpoint from the other Small IN100 example category.

```powershell
$runner = 'C:\path\to\nxrunner.exe'
$suite = 'C:\path\to\Small_IN100_Distribution_Examples'
Set-Location 'C:\path\to\writable\BoundaryWork'
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Distribution_Examples' | Out-Null
& $runner --execute (Join-Path $suite '(01) Small IN100 Boundary Distribution Comparison.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Boundary distribution example failed' }
```

The pipeline preserves the imported network, removes 12 degenerate faces into a separate mesh, recalculates its areas/normals, and saves seven result files. It uses the supplied checked boundary-pair table instead of regenerating it through Shared Feature Face. No other example is a prerequisite.

## Source and interpretation

The source is an existing public simplnx validation dataset derived from Small IN100, with 795 feature-table rows and 4,224 distinct positive-label internal pairs. Its embedded recipe records conversion from a legacy boundary-analysis file. Exact identity with the original research mesh and physical calibration are not established. In particular, the converted `Meter` metadata must not be treated as independent calibration evidence. This example uses normalized distributions and does not report absolute physical areas.

[Glowinski and Morawiec (2014)](https://doi.org/10.1007/s11661-014-2325-y) supplies the distribution-method comparison. The example is **illustrative**, not reproduction of the published reconstruction or peaks. [SourceDataLicense.md](SourceDataLicense.md) distinguishes article rights from dataset redistribution permission. The native preview displays saved values; no paper image is copied.

`PythonGeneration.yaml` describes temporary conversion through the matching application environment. OOC, generated Python, GUI use, and materials interpretation are separate validation tasks.
