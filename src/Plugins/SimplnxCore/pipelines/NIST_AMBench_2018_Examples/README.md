# NIST AM-Bench 2018 contour inspection

One combined example prepares local inspection points from the public AMB2018-01 bridge CAD. Read [(01) NIST Bridge Contour Inspection](%2801%29%20NIST%20Bridge%20Contour%20Inspection.md) for choices, outputs and checks. This is nominal design geometry, not measured material data or a manufacturing toolpath.

## Setup and run

1. Choose a writable working folder. Download the original [NIST binary STL](https://s3.amazonaws.com/nist-midas/1889/AMB2018_01_Part.STL) to `Data/NIST_AMBench_2018_Examples/AMB2018_01_Part.STL` beneath it. This 18,884-byte file is not yet included in the normal application data package.
2. Check SHA256 `2ceda46b2b840b39c7c41bc91436087e412ac43b52b0a5d106bbb50060a7c631` with `Get-FileHash -Algorithm SHA256` on Windows. It matches NIST's [published checksum](https://s3.amazonaws.com/nist-midas/1889/AMB2018_01_Part.STL.sha256). SHA512 and provenance are in [DataSources.json](DataSources.json).
3. Create `Data/Output/NIST_AMBench_2018_Examples`. Run the pipeline from that working folder with SimplnxCore loaded. In the GUI, set absolute input/output file parameters if relative paths resolve elsewhere.

```text
nxrunner --preflight "<pipeline-folder>/(01) NIST Bridge Contour Inspection.d3dpipeline"
nxrunner --execute "<pipeline-folder>/(01) NIST Bridge Contour Inspection.d3dpipeline"
```

The result is `Data/Output/NIST_AMBench_2018_Examples/BridgeContourInspection.dream3d`. It contains the original CAD, all sections, cropped sections, selected contour and sample points. Native Windows in-memory execution is checked; materials interpretation remains subject to expert review.

The CTest example stages the original input from the application cache or downloads it with checksum verification. That test download does not add the dataset to the installer. Keep [NIST attribution and reuse terms](SourceDataLicense.md) with shared data and derived results.
