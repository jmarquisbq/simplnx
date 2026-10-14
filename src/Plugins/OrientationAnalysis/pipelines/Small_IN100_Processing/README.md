# Small IN100 processing

The existing numbered pipelines form a staged reconstruction and analysis workflow. Pipeline **01** now has a companion guide explaining a single-scan import inspection and the original archive operation. Pipelines **02-09** remain the existing legacy examples; their execution order and content are unchanged.

## Run the archive and inspection

Use a writable working folder whose `Data/Small_IN100/` contains **all 117 files**, `Slice_1.ang` through `Slice_117.ang`. These are the normal SmallIN100 example data. If needed, obtain the public [Small_IN100.tar.gz archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) and extract its `Small_IN100/` folder under `Data/`.

```powershell
Set-Location 'C:\path\to\writable\working-folder'
New-Item -ItemType Directory -Force -Path 'Data\Output\Small_IN100_Processing\Inspection', 'Data\Output\Reconstruction' | Out-Null
& 'C:\path\to\nxrunner.exe' --preflight 'C:\path\to\pipelines\Small_IN100_Processing\(01) Small IN100 Archive.d3dpipeline'
if ($LASTEXITCODE -ne 0) { throw 'Archive preflight failed.' }
& 'C:\path\to\nxrunner.exe' --execute 'C:\path\to\pipelines\Small_IN100_Processing\(01) Small IN100 Archive.d3dpipeline'
if ($LASTEXITCODE -ne 0) { throw 'Archive execution failed.' }
```

For the GUI, open pipeline 01 with **File > Open**. If relative paths do not resolve, set the file input/output parameters to absolute paths. The pipeline saves a one-scan inspection under `Data/Output/Small_IN100_Processing/Inspection/` and the existing archive under `Data/Output/Reconstruction/Small_IN100.h5ebsd`. Read the [archive companion](%2801%29%20Small%20IN100%20Archive.md) before interpreting the frame corrections, quality mask or 2D placeholder Z spacing. Continue with the existing full reconstruction or companion-backed feature-preparation workflow only after the archive is complete.

## Source and scope

The archive is declared by OrientationAnalysis CMake with SHA-512:

```text
79e9f6948d4e8e06187e11216a67596fa786ffd2700e51f594ad014090383eb8bcc003e14de2e88082aa9ae512cc4fc9cee22c80066fc54f38c3ebc75267eb5b
```

It contains 117 measured ANG scans, 189 by 201 pixels per section, with 0.25 micrometer XY step and established 0.25 micrometer section pitch. The [official tutorial](https://dream3d.bluequartz.net/Help/2_Tutorials/EBSDReconstruction/) attributes the data to M. Uchic and colleagues at AFRL. The [Groeber and Jackson paper](https://doi.org/10.1186/2193-9772-3-5) provides scientific context; the inspection is illustrative and claims no numerical paper reproduction. Raw-source identity with the publisher's processed supplement and separate archive redistribution terms have not been established. No raw or processed volume files are included here; the preview is derived from native output.

`FilterCoverage.json` and `PythonGeneration.yaml` describe only companion-backed pipeline 01. The `.d3dpipeline` is authoritative. Generated Python, GUI and out-of-core execution need separate validation.
