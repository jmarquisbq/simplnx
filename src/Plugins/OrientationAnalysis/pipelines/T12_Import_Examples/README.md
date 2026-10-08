# T12 data import examples

These two independent examples consume the outputs of [(03) T12 Compact Report](../T12_Reporting_Examples/%2803%29%20T12%20Compact%20Report.md). Run that reporting pipeline first. Then run either import pipeline from the same writable working directory; neither import requires the other. The `.d3dpipeline` files are executable authority, and the Markdown and JSON-compatible YAML explain the inputs and choices.

> **Before adapting:** Check the paper-linked data's provenance, the map spacing and phase symmetry, the validity masks, and the segmentation and reporting rules. These pipelines illustrate reporting decisions for one map; they do not establish a material acceptance threshold or reproduce the paper's numerical results.

The predecessor writes `Data/Output/T12_Reporting_Examples/Report/T12_CompactReport.csv` and `T12_CompactReport.dream3d` in the same directory. The CSV has a seven-column header and 3059 positive `Feature_ID` rows; it omits background row 0. The DREAM3D file also has the original `T12` image and cell arrays, plus a separate `Reporting/Cell Feature Data` Attribute Matrix with 3060 rows including background. Set absolute paths in the GUI if its working directory differs from the CLI folder.

```powershell
$work = 'C:\DREAM3D\T12Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines\T12_Import_Examples'
Set-Location $work
& $runner --execute (Join-Path $examples '(01) T12 Typed CSV Import.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Typed CSV import failed; inspect runner output.' }
& $runner --execute (Join-Path $examples '(02) T12 Selective DREAM3D Import.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Selective DREAM3D import failed; inspect runner output.' }
```

[(01) Typed CSV Import](%2801%29%20T12%20Typed%20CSV%20Import.md) reads the table into a 3059-row `ImportedCSV` Attribute Matrix, parses numeric columns explicitly, and saves a masked GOS summary with the imported arrays. CSV row position is not the original feature ID: the first imported tuple holds `Feature_ID=1`.

[(02) Selective DREAM3D Import](%2802%29%20T12%20Selective%20DREAM3D%20Import.md) imports only `Reporting` and descendants from the complete checkpoint. Its feature Attribute Matrix preserves the 3060-row shape and source types, including row 0. It saves a separate masked summary. The original image geometry is deliberately absent from this result.

The measured map is the public T12 archive associated with [Allain-Bonasso et al. (2012)](https://doi.org/10.1016/j.msea.2012.03.068). The archive's `PublicRelease.txt` attributes permission for public release and DREAM.3D distribution to Francis Wagner; it names no standard license. Follow the [orientation suite setup](../T12_Orientation_Examples/README.md) for acquisition and provenance. These import operations extend the NX reporting lesson; they do not reproduce an import method or numerical result from the paper.

Both imports retain the same 1,443 reportable grains and mean GOS **2.325059°**. CSV requires explicit types and an ID column; its Float32 values can change slightly through decimal text. Structured import preserves all six source arrays exactly, including the Boolean mask and background row. The companion figures use the actual imported arrays.

If the upstream cohort or measurements change, regenerate the compact report, check the CSV's actual header and row count, update the CSV tuple dimension if needed, and rerun the desired import. `PythonGeneration.yaml` describes optional temporary Python conversion.

Both examples passed Windows in-memory preflight/execution and independent output checks. GUI use, OOC, generated Python and full application installation remain unverified; upstream measurement validity is a separate question.
