# Ti64 image cleanup examples

Use a measured Ti-6Al-4V micrograph to examine how preprocessing choices affect image contrast and a later intensity selection. The examples preserve the source image and explain the choices, alternatives, and limitations in the companions. They do not assign grain or phase identities.

## Data and run instructions

Download the public [ImageProcessing example archive](https://www.dream3d.io/Data_Archive/ImageProcessing_Examples_v1.tar.gz) and extract it into a writable working folder's `Data/` directory. The required image is `Data/ImageProcessing_Examples/materials/microstructure_grayscale.png`; installed example data may already provide it. Preserve the archive's `Provenance.json` for source identities and transformations.

```powershell
$work = 'C:\DREAM3D\Ti64Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines\ImageProcessing\Ti64_Image_Cleanup_Examples'
New-Item -ItemType Directory -Force -Path "$work\Data" | Out-Null
tar -xzf 'C:\path\to\ImageProcessing_Examples_v1.tar.gz' -C "$work\Data"
if ($LASTEXITCODE -ne 0) { throw 'Data extraction failed.' }
Set-Location $work
& $runner --execute (Join-Path $examples '(01) Ti64 Image Preparation.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Preparation failed; inspect the runner output.' }
```

Relative paths resolve from that CLI working folder. In the GUI, open the pipeline and set absolute file input/output paths if your launch location does not resolve `Data/...`. Use a short working path on Windows to leave room for temporary output filenames.

## Workflow

[(01) Image Preparation](%2801%29%20Ti64%20Image%20Preparation.md) retains the full 604 × 604 raster and creates a 596 × 596 crop after removing a four-pixel margin. It converts cropped intensities to floating point and establishes a common 0–1 range. The saved checkpoint is `Data/Output/Ti64_Image_Cleanup_Examples/Preparation/Ti64_Prepared.dream3d`.

Spatial coordinates are uncalibrated pixels with spacing 1 and unit choice **Unknown**. Intensity is relative image brightness. Neither represents a physical phase identity, material acceptance threshold, or calibrated length. The crop is a documented example choice, not a paper-prescribed region or a representative-volume claim.

## Source and paper relationship

The input is a grayscale conversion of `Images1/image_500.png` from [Arun Baskaran's public repository](https://github.com/ArunBaskaran/Image-Driven-Machine-Learning-Approach-for-Microstructure-Classification-and-Segmentation-Ti-6Al-4V), copyright 2019 ArunBaskaran, under MIT terms. The full notice is retained in [SourceDataLicense.txt](SourceDataLicense.txt); the result figures are derived from that image. Raw data are obtained separately.

[Fotos et al. (2023)](https://doi.org/10.1007/s10853-023-08901-w) provides the microstructural-analysis context. Its HADMA workflow uses learned class/boundary predictions followed by watershed. This set is **illustrative**: it studies native NX preprocessing choices, with no trained classifier or reproduction of the paper's measurements. Each companion explains the relevant differences and what an extension would require.

The `.d3dpipeline` files define the executable steps; matching Markdown and JSON-compatible YAML explain them. `FilterCoverage.json` indexes selected filters. `PythonGeneration.yaml` describes temporary Python generation, which has not been validated for this suite.

## Execution scope

Reference results were checked with the Windows CLI and in-memory storage. Scientific interpretation, native GUI use, out-of-core storage, and generated Python execution remain pending separate review or verification.
