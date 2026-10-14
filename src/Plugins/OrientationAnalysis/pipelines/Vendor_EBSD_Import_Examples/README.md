# Vendor EBSD import examples

One combined [Vendor EBSD Import Checks](%2801%29%20Vendor%20EBSD%20Import%20Checks.md) pipeline demonstrates two real vendor formats while keeping the specimens separate. It teaches calibration and tuple-order checks, phase-valid masks, vendor error-code differences and supplementary HDF5 columns before analysis. The Bruker branch intentionally reports an unresolved spatial-order prerequisite; it is an import diagnostic, not a ready-to-segment map.

## Setup and run

Choose a writable working folder and create `Data/Vendor_EBSD_Import_Examples/` and `Data/Output/Vendor_EBSD_Import_Examples/Inspection/` beneath it. Download these official archives and copy only the named original members into the input folder:

| Public archive | Member to copy |
| --- | --- |
| [6_6_ImportH5Data.tar.gz](https://github.com/BlueQuartzSoftware/simplnx/releases/download/Data_Archive/6_6_ImportH5Data.tar.gz) | `6_6_ImportH5Data/H5EspritReaderTest.h5` (29,905,456 bytes) |
| [H5Oina_Test_Data.tar.gz](https://github.com/BlueQuartzSoftware/simplnx/releases/download/Data_Archive/H5Oina_Test_Data.tar.gz) | `H5Oina_Test_Data/H5Oina_Test_Data.h5oina` (203,434,350 bytes) |

Verify each file's SHA256 against [DataSources.json](DataSources.json), for example with `Get-FileHash -Algorithm SHA256`. The Oxford file contains a large detector-pattern dataset, although this pipeline reads only scalar map fields. Keep the original file unchanged. Existing build/test caches may supply these bytes; neither file is currently added to normal package installation by this example.

With SimplnxCore and OrientationAnalysis loaded, run from the chosen working folder:

```text
nxrunner --preflight "<pipeline-folder>/(01) Vendor EBSD Import Checks.d3dpipeline"
nxrunner --execute "<pipeline-folder>/(01) Vendor EBSD Import Checks.d3dpipeline"
```

For GUI use, set absolute input/output paths if relative `Data/...` paths do not resolve. The output is `Inspection/VendorEBSD_ImportChecks.dream3d`, plus one Bruker and one Oxford CSV report. Bruker XY is **uncalibrated despite its fallback unit label**; read the guide before interpreting geometry.

The native Windows in-memory run and independent input/summary checks are verified. GUI, OOC, generated Python and materials interpretation remain unverified. The source `.d3dpipeline` is executable authority; its YAML captures structured constraints and temporary Python-generation instructions.
