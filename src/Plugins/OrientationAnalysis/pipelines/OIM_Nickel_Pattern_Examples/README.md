# OIM nickel detector-pattern example

One combined example links a measured EBSD map to the detector patterns behind two neighboring Confidence Index values. It imports, selects, exports, and retains context in seven steps.

| Pipeline | Task | Main outputs |
|---|---|---|
| [(01) OIM Nickel Pattern Inspection]((01)%20OIM%20Nickel%20Pattern%20Inspection.md) | Inspect the adjacent acquisition cells (67,10) and (68,10) without changing detector intensities or interpreting specimen orientations. | Full scalar-map context, two selected patterns, a raw binary stream, and a two-row manifest. |

Run from a writable working folder containing `Data/OrientationAnalysis/EdaxOIMData.h5`. Results go to `Data/Output/OIM_Nickel_Pattern_Examples/Inspection/`. GUI launches may use a different working folder; set absolute file paths if needed.

The measured nickel sample comes from the official [6_6_ImportH5Data archive](https://github.com/BlueQuartzSoftware/simplnx/releases/download/Data_Archive/6_6_ImportH5Data.tar.gz), member `6_6_ImportH5Data/EdaxOIMData.h5`. Existing build rules copy it into runtime data, but do not install it in the normal application package. No input data is added or uploaded by this category. Public distribution is established; no separate dataset license or additional redistribution permission was found. Source identity, checksums, and packaging status are in `DataSources.json`.

This is an illustrative inspection/export workflow, not reproduction of a paper or an EBSD acceptance rule. CI contrast motivates examining the patterns; it does not prove correct indexing, noise, strain, or a grain boundary. Acquisition-grid coordinates and detector-pixel coordinates remain separate. The existing `EBSD_File_Processing/ImportEdaxOIMData.d3dpipeline` is unchanged and has a different input and purpose.

The `.d3dpipeline` is the executable authority; its same-stem guide explains choices, and YAML records exact arguments. `FilterCoverage.json` records all seven filter occurrences. `PythonGeneration.yaml` specifies temporary generation from the executable; Python and OOC execution have not been verified.
