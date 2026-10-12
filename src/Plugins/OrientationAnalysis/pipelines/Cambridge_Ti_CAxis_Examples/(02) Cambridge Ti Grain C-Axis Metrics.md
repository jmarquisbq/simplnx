# Cambridge Ti Grain C-Axis Metrics

Executable pipeline: `(02) Cambridge Ti Grain C-Axis Metrics.d3dpipeline`

> **Before adapting:** **Validation warning:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

Run stage 01 first, then read its full checkpoint. Use this stage to compare c-axis variation **within** full-orientation grains and across touching grains. The published author script uses a 7° `calcGrains` tolerance on indexed pixels; this example uses current NX face-connected EBSD segmentation at 7° and makes no identity claim against MTEX grains.

## At a glance

| Item | Value |
| --- | --- |
| Input | `Data/Output/Cambridge_Ti_CAxis_Examples/Prepared/CambridgeTi_Prepared.dream3d` |
| Output | `Data/Output/Cambridge_Ti_CAxis_Examples/GrainMetrics/CambridgeTi_GrainMetrics.dream3d` |
| Native labels | `GrainIds` for positive measured grains; row 0 is background |
| Native measures | `Areas`, `NumCells`, `ReportingGrains`, `AvgCAxes`, `CAxisDeviationFromGrain`, `MeanCAxisDeviation`, `StdDevCAxisDeviation`, neighbor c-axis angles |
| Preview | [Grain c-axis deviation](CambridgeTiGrainCAxisDeviation.png) from native cells |

## Purpose and real-world setting

The titanium case in the paper links orientation relationships with spatial boundary traces. This NX extension asks where the projected c axis is stable inside grains and how c-axis directions differ between neighbors. The projected c axis does not retain the full orientation, including rotation about c; the saved scalar angles discard further directional information. These descriptors cannot identify twin relationships.

## Data flow and filter choices

`ReadDREAM3DFilter` loads the complete stage-01 checkpoint. `EBSDSegmentFeaturesFilter` uses masked `AnalysisPhases` and `Quats`, 7° full-orientation tolerance, face adjacency (four neighbors in the single measured layer), no periodic wrap, and stable nonrandomized IDs. `ComputeFeatureSizesFilter` writes `NumCells`, `Areas`, and equivalent **circle** diameters for every positive grain. At 0.25 × 0.25 µm and unit software z, `Areas = NumCells × 0.0625 µm²`; avoid calling them 3D volumes. `MultiThresholdObjectsFilter` marks `ReportingGrains` where `Areas > 1 µm²`, matching the author's grain display threshold without deleting or relabeling smaller grains. `ComputeFeaturePhasesFilter` assigns feature phases. `ComputeAvgOrientationsFilter` writes symmetry-aware `AvgQuats`, while `ComputeFeatureNeighborsFilter` creates face-contiguous `NeighborList` and `SharedBoundaryLength` (0.25 µm per in-plane edge under unit z).

`ComputeAvgCAxesFilter` normalizes a sign-aligned accumulation of member-cell c axes. `ComputeFeatureReferenceCAxisMisorientationsFilter` compares each indexed cell with its grain `AvgCAxes`, then calculates the per-grain mean and population standard deviation in degrees. `ComputeFeatureNeighborCAxisMisalignmentsFilter` instead projects c axes from each grain's **AvgQuats** and computes acute pairwise angles plus an equal-neighbor-weighted mean. `AvgCAxes` and the c axis projected from `AvgQuats` are distinct estimators. `WriteDREAM3DFilter` saves the complete checkpoint.

## Outputs and interpretation

The registered native Release run retained 1,285 populated grains and conserved the 164,274 indexed cells (total measured area 10,267.125 µm²). Of these grains, 861 contain just one measured pixel. The `Areas > 1 µm²` cohort contains 70 grains yet covers about 98.595% of indexed pixels. The displayed **pixel-weighted** cell-deviation median is about 0.713° and the 95th percentile about 2.056°; a mean across one value per grain would weight each tiny grain equally and answer a different question. These values describe this segmentation and reference choice, not material strain. The preview masks excluded pixels and caps its color display at the 99th percentile while retaining uncapped values in the checkpoint. Background cell zeros are placeholders; feature row zero and any undefined no-neighbor means are excluded from summaries.

## Adaptation, alternatives, and pitfalls

Change the full-orientation tolerance only with a corresponding grain-size and boundary review. Direct c-axis segmentation in stage 03 answers a different question because rotations about c can be grouped together. Avoid averaging Euler components or equating a quaternion-derived c-axis direction with `AvgCAxes`. `ReportingGrains` is a display cohort only; use all positive grains in the neighbor graph and grouping stage. Verify positive grain `Areas` against pixel counts, `AvgCAxes` norms, and the cell-to-grain deviation formula before drawing conclusions.

## Scientific basis and assistant guidance

[Krakow et al. (2017), *On three-dimensional misorientation spaces*](https://doi.org/10.1098/rspa.2017.0274) uses this original alpha-Ti EBSD case in its full misorientation and twin-boundary analysis. The [Cambridge companion dataset](https://doi.org/10.17863/CAM.8815) supplies the original Oxford CPR/CRC pair and author `section4b.m` script under CC BY 4.0. Credit Robert Krakow and coauthors and the Cambridge repository when adapting or redistributing derived work. This example is **illustrative**: it does not reproduce MTEX grain identifiers, full three-parameter misorientation clusters, twin classifications, or figure-10 pixels.

For an LLM or MCP assistant: explain the 7° author-script link as a tolerance choice only; preserve the two c-axis estimators, positive-grain mask, and 2D area units. The [suite README](README.md) lists predecessors and provenance.
