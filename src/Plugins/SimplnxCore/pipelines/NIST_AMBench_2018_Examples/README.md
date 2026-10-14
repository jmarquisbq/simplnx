# NIST AM-Bench 2018 geometry examples

These examples use public AMB2018-01 nominal bridge CAD. They prepare inspection geometry and demonstrate part-file interchange; they do not contain measured deformation or manufacturing instructions.

| Example | Purpose and dependency |
| --- | --- |
| [(01) Contour Inspection](%2801%29%20NIST%20Bridge%20Contour%20Inspection.md) | Slice, crop, sample and compare CAD distances; independent of 02-03. |
| [(02) Part Export](%2802%29%20NIST%20Bridge%20Part%20Export.md) | Position four nominal copies and write separate binary STL files. |
| [(03) STL Assembly](%2803%29%20NIST%20Bridge%20STL%20Assembly.md) | Combine the completed 02 exports, preserving file identity and geometry. |

## Setup and run

1. Choose a writable working folder. Download the original [NIST binary STL](https://s3.amazonaws.com/nist-midas/1889/AMB2018_01_Part.STL) to `Data/NIST_AMBench_2018_Examples/AMB2018_01_Part.STL` beneath it. This 18,884-byte file is not yet included in the normal application data package.
2. Check SHA256 `2ceda46b2b840b39c7c41bc91436087e412ac43b52b0a5d106bbb50060a7c631` with `Get-FileHash -Algorithm SHA256` on Windows. It matches NIST's [published checksum](https://s3.amazonaws.com/nist-midas/1889/AMB2018_01_Part.STL.sha256). SHA512 and provenance are in [DataSources.json](DataSources.json).
3. Create `Data/Output/NIST_AMBench_2018_Examples`. For 02-03, also create its `Parts` and `Assembly` subfolders and keep `Parts` free of unrelated STL files. Run from that working folder with SimplnxCore loaded. In the GUI, set absolute file/directory parameters if relative paths resolve elsewhere.

```text
nxrunner --preflight "<pipeline-folder>/(01) NIST Bridge Contour Inspection.d3dpipeline"
nxrunner --execute "<pipeline-folder>/(01) NIST Bridge Contour Inspection.d3dpipeline"

nxrunner --execute "<pipeline-folder>/(02) NIST Bridge Part Export.d3dpipeline"
nxrunner --preflight "<pipeline-folder>/(03) NIST Bridge STL Assembly.d3dpipeline"
nxrunner --execute "<pipeline-folder>/(03) NIST Bridge STL Assembly.d3dpipeline"
```

Example 01 writes `BridgeContourInspection.dream3d`. Examples 02-03 write `Assembly/NISTBridge_PartLayout.dream3d` and `Assembly/NISTBridge_STLAssembly.dream3d`, respectively, beneath the same output root. The four generated STL files are intermediate products; only the original downloaded CAD is a source-data dependency. Complete 02 before preflighting 03 because the importer requires existing files.

The CTest example stages the original input from the application cache or downloads it with checksum verification. That test download does not add the dataset to the installer. Keep [NIST attribution and reuse terms](SourceDataLicense.md) with shared data and derived results.
