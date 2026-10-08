# Ti64 segmentation examples

Explore connected bright regions in the measured Ti-6Al-4V image prepared by [Ti64 Image Cleanup Examples](../Ti64_Image_Cleanup_Examples/README.md). These pipelines compare image connectivity and a minimum pixel-count rule. They describe brightness patches, not grains, phases, or a reproduced scientific classifier.

## Data and run instructions

Download the public [ImageProcessing example archive](https://www.dream3d.io/Data_Archive/ImageProcessing_Examples_v1.tar.gz) and extract it into a writable working folder's `Data/` directory. The cleanup suite requires `Data/ImageProcessing_Examples/materials/microstructure_grayscale.png`. Preserve the archive's `Provenance.json` and the MIT notice in [SourceDataLicense.txt](SourceDataLicense.txt). Run cleanup stages 01, 02, and 03 first to create `Data/Output/Ti64_Image_Cleanup_Examples/Threshold/Ti64_ThresholdSensitivity.dream3d`.

```powershell
$work = 'C:\DREAM3D\Ti64Work'
$runner = 'C:\path\to\nxrunner.exe'
$cleanup = 'C:\path\to\pipelines\ImageProcessing\Ti64_Image_Cleanup_Examples'
$examples = 'C:\path\to\pipelines\ImageProcessing\Ti64_Segmentation_Examples'
Set-Location $work
foreach ($name in @('(01) Ti64 Image Preparation.d3dpipeline', '(02) Ti64 Smoothing Comparison.d3dpipeline', '(03) Ti64 Threshold Sensitivity.d3dpipeline')) {
    & $runner --execute (Join-Path $cleanup $name)
    if ($LASTEXITCODE -ne 0) { throw "Cleanup predecessor failed: $name" }
}
& $runner --execute (Join-Path $examples '(01) Ti64 Connected Bright Regions.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Connected bright regions failed.' }
& $runner --execute (Join-Path $examples '(02) Ti64 Connectivity Comparison.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Connectivity comparison failed.' }
& $runner --execute (Join-Path $examples '(03) Ti64 Minimum Size Cleanup.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'Minimum size cleanup failed.' }
```

Stages 02 and 03 both read stage 01's checkpoint. They do not read each other's outputs, so either order works after 01. Relative file paths resolve from the CLI working folder. In the GUI, set absolute paths if `Data/...` does not resolve. Use a short writable working path on Windows for generated temporary filenames.

## Workflow

[(01) Connected Bright Regions](%2801%29%20Ti64%20Connected%20Bright%20Regions.md) labels four-neighbor connected patches in `BrightOriginal`. It saves the `uint32` labels, `int32` indexing copy, per-region intensity statistics, and a positive-ID CSV under `Connected/`.

[(02) Connectivity Comparison](%2802%29%20Ti64%20Connectivity%20Comparison.md) starts from stage 01 and adds eight-neighbor labels and matching statistics under `Connectivity/`. Both label images use the same foreground. Region IDs are local to each branch.

[(03) Minimum Size Cleanup](%2803%29%20Ti64%20Minimum%20Size%20Cleanup.md) starts from stage 01, flags original face regions with at least 25 pixels, then creates a separate relabeled image dropping smaller regions to background. It saves cleaned statistics and CSV under `Cleanup/`. The threshold is an example choice in uncalibrated pixel units.

The source image and earlier geometries and arrays remain present in every checkpoint. All three examples use the 596 × 596 crop in pixel coordinates and normalized brightness. No new physical measurement or material phase classification is implied.

## Source and paper relationship

The grayscale raster derives from `Images1/image_500.png` in [Arun Baskaran's public repository](https://github.com/ArunBaskaran/Image-Driven-Machine-Learning-Approach-for-Microstructure-Classification-and-Segmentation-Ti-6Al-4V), copyright 2019 ArunBaskaran under MIT terms. [Fotos et al. (2023)](https://doi.org/10.1007/s10853-023-08901-w) provide context for Ti-6Al-4V image analysis. Their learned class/boundary predictions and watershed are outside this illustrative segmentation set. Each companion gives the comparison and extension requirements.

The `.d3dpipeline` files are executable authority. Matching Markdown and JSON-compatible YAML explain their choices; `FilterCoverage.json` indexes the selected filters. `PythonGeneration.yaml` specifies temporary Python generation.

All three examples passed Windows in-memory preflight and execution. Independent checks matched connected-region membership, size cleanup, CSVs, and statistics within Float32 tolerance. GUI use, OOC, generated Python, and full application installation have not been verified. Scientific suitability for a new material or image remains a separate review.
