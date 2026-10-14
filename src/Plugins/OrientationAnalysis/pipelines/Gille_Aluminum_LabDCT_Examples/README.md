# Gille aluminium LabDCT examples

The combined [LabDCT Preparation](%2801%29%20Gille%20Aluminum%20LabDCT%20Preparation.md) pipeline imports a public measured AA6016 T4 grain-map crop, handles orientation conventions, and reports observed grain sizes with crop-boundary flags. It is preparation for further analysis, not a reproduction of the associated deformation study.

## Setup and run

1. Choose a writable working folder. Download the original [GrainMapper3D HDF5](https://data.materialsdatafacility.org/mdf_open/0989ee07-e015-494d-8805-8e2703e76763/1.0/sample_S_undeformed_6_grain_center_slice.h5) to `Data/Gille_Aluminum_LabDCT_Examples/sample_S_undeformed_6_grain_center_slice.h5` beneath it. The source [MDF record](https://doi.org/10.18126/yc1m-nk41) documents the dataset and CC BY 4.0 terms.
2. Check **567,693,056 bytes** and SHA256 `8802bbe5413f766330ac394c150a8d75818dd458d9e47123b05b7c8dc53cb522`, for example with `Get-FileHash -Algorithm SHA256`. A partial download is not usable. The original file includes a large absorption-CT volume; this example reads only LabDCT.
3. Create `Data/Output/Gille_Aluminum_LabDCT_Examples/`. With SimplnxCore and OrientationAnalysis loaded, run from the working folder:

```text
nxrunner --preflight "<pipeline-folder>/(01) Gille Aluminum LabDCT Preparation.d3dpipeline"
nxrunner --execute "<pipeline-folder>/(01) Gille Aluminum LabDCT Preparation.d3dpipeline"
```

For GUI use, set absolute file paths if relative `Data/...` paths do not resolve. The 14-step pipeline writes `GilleAluminum.dream3d`, `GilleAluminum_Grains.csv`, `GilleAluminum_FFT.txt` and `GilleAluminum_SPPARKS.txt`. Read the companion before interpreting colour keys, millimetre units, crop-boundary sizes, source preprocessing or the two distinct export contracts. The text files are microstructure data, not complete or validated solver setups.

The `.d3dpipeline` is executable authority. YAML records constraints and verified reference values; `PythonGeneration.yaml` describes temporary generation. Native Windows in-memory execution and independent numeric checks are verified. GUI, OOC, generated Python and scientific interpretation remain unverified.

Raw data is not installed by this category. The registered test accepts the original file through the optional `SIMPLNX_GILLE_LABDCT_EXAMPLE_FILE` CMake cache setting or the runtime `Data/Gille_Aluminum_LabDCT_Examples/` location. It reports a skip when neither is available, and rejects a configured missing file or checksum mismatch.
