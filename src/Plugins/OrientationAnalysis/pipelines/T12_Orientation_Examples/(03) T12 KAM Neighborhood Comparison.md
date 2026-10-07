# T12 KAM Neighborhood Comparison

Executable pipeline: `(03) T12 KAM Neighborhood Comparison.d3dpipeline`

> **Before adapting:** This example is configured for the T12 preparation checkpoint. Review phase symmetry, validity, resolution, grain definition, and reporting population before changing inputs. Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

## At a glance

- **Input:** The complete `Data/Output/T12_Orientation_Examples/Preparation/T12_Orientations.dream3d` checkpoint from [(01) T12 Orientation Preparation](%2801%29%20T12%20Orientation%20Preparation.md).
- **Result:** Three aligned cell KAM maps, three one-row indexed-pixel summaries, and one DREAM3D file that preserves the preparation arrays.
- **Main choice:** Change the radius while holding grain grouping fixed, then change grouping while holding the radius fixed.
- **Run order:** Run preparation first, then this pipeline from the folder containing `Data/Output`; see the [suite README](README.md). Pipeline (02) is independent. In the GUI, set absolute paths if relative paths do not resolve.

## Purpose and real-world setting

An electron backscatter diffraction (EBSD) map measures crystal orientation at each surface pixel. Kernel average misorientation (KAM) compares a center orientation with qualifying orientations nearby. Changing the window or admission rule changes the question. [Allain-Bonasso et al. (2012), DOI 10.1016/j.msea.2012.03.068](https://doi.org/10.1016/j.msea.2012.03.068) studied local orientation variation in IF steel. This example uses a public paper-linked T12 map; it does not reproduce the paper's noise reduction, matched deformation maps, or results. The file name does not establish deformation state.

## Data flow and choices

| Stage | Why this choice and what to watch for |
|---|---|
| Read full checkpoint | Reuse `T12`, `Quats`, `Phases`, `FeatureIds`, cubic `CrystalStructures`, and `IndexedPixels`. Preparation converted Euler angles to radians before forming quaternions. Symmetry-aware KAM outputs degrees. Each filter creates a distinct cell array; source arrays remain available. |
| `KAM_R1_Grain` | Radius **[1, 1, 0]** gives a clipped 3 × 3 window in this measured slice. `use_feature_ids=true` admits neighbors with the center's positive feature ID. Preparation's phase-separated segmentation makes those neighbors phase-coherent; the filter does not separately test their phase in this mode. This is the baseline for both comparisons. |
| `KAM_R2_Grain` | Radius **[2, 2, 0]** gives a clipped 5 × 5 window with the same grain rule. Comparing it with `KAM_R1_Grain` changes spatial reach only. More distant cells can raise or lower an individual mean; a larger radius need not increase every pixel's KAM. Edges, invalid gaps, and grain boundaries change the number of qualifying cells. |
| `KAM_R1_Phase` | Radius returns to **[1, 1, 0]**, but `use_feature_ids=false` admits other **positive** feature IDs in the center's phase. Comparing it with `KAM_R1_Grain` isolates grouping. Across-boundary angles change the neighborhood question rather than improve the same quantity. Phase-zero and feature-zero cells are excluded. Neither variant changes grain labels. |
| Three Attribute Array Statistics | Each one-row matrix, `T12/KAM_R1_GrainSummary`, `T12/KAM_R2_GrainSummary`, or `T12/KAM_R1_PhaseSummary`, masks its cell map with `T12/Cell Data/IndexedPixels`. Each records `Length`, `Minimum`, `Maximum`, `Mean`, `Median`, and population `StandardDeviation` (denominator N). Pixels in grains smaller than four cells are included. Pipeline (02)'s `GrainGOSSummary` instead uses `ReportableFeatures` and weights each reportable grain once. |
| Write DREAM3D | Save the three maps, summaries, and full checkpoint to `Data/Output/T12_Orientation_Examples/Neighborhoods/T12_NeighborhoodComparison.dream3d`, using compression level 5 and no XDMF sidecar. The maps and summaries are the stable outputs; no CSV is needed. |

## Why this differs from the paper

The NX square windows include the center pixel with a zero angle and diagonals. The paper's Equation 3 uses **four surrounding pixels**. We use the built-in NX filter to compare its supported radius and grain-grouping choices on one fixed input. The larger window and across-grain variant are teaching comparisons, not settings attributed to the paper or proposed improvements to its method.

To reproduce a four-neighbor calculation, add a validated script or filter selecting only the axial neighbors. No radius setting in this filter gives that cross-shaped neighborhood with the center excluded. Confirm how the intended method treats invalid pixels, grain boundaries and map edges, and normalize by its admitted-neighbor rule. Test the added calculation on hand-checkable orientations before applying it to T12. Multiplying the square-window output by a constant cannot remove its diagonal contributions.

Noise reduction and matched deformation maps are also omitted: this branch holds the supplied preparation fixed so changes can be attributed to neighborhood choices. If preprocessing changes, regenerate all three maps from the same revised checkpoint. If comparing deformation states, establish the scan states and spatial/grain correspondence first; feature numbers are not tracking IDs.

A valid pixel with no other qualifying neighbor can have zero KAM, so zero alone is not proof of an undeformed or perfectly uniform region. The **below-5°** criterion in preparation defines grain segmentation; it is not an extra neighbor-angle cutoff applied by these KAM steps.

## Outputs and interpretation

The checkpoint's 0.5 µm XY spacing gives radius one an axial reach of 0.5 µm and radius two an axial reach of 1.0 µm; diagonals reach farther. Z radius zero confines comparisons to the one measured slice. A different acquisition step changes these physical distances even if the integer radius stays the same.

Reference results for this prepared map:

| Indexed-pixel map | Length | Mean (°) | Population SD (°) | Maximum (°) |
| --- | ---: | ---: | ---: | ---: |
| `KAM_R1_Grain` | 1,038,142 | 0.57397 | 0.26895 | 7.18223 |
| `KAM_R2_Grain` | 1,038,142 | 0.82345 | 0.40364 | 7.86957 |
| `KAM_R1_Phase` | 1,038,142 | 1.20792 | 2.72605 | 39.93160 |

![T12 three-way KAM neighborhood comparison preview](T12NeighborhoodComparison.png)

The figure shows the same 400 × 400 pixel crop (`x=400:800`, `y=250:650`) for all three maps on a common 0–5° display scale; values above 5° are saturated in the image, not in the saved arrays. Black marks excluded pixels; valid zeros use the low end of the color scale. Calculations and summaries use the **full map**. `Length` should match the `IndexedPixels` count. Changes can reflect admitted-neighbor counts near boundaries and gaps. These outputs do not establish strain, geometrically necessary dislocation density (GND), or the paper's grain average misorientation (GAM). The archive notice attributes public distribution permission to Francis Wagner but names no standard license.

For an LLM or MCP assistant: check phase symmetry, the validity mask, pixel spacing, grain definition, and desired statistical population before recommending a variant. Treat `.d3dpipeline` arguments as executable authority. Describe radius and grouping changes separately, and limit claims about physical deformation or paper agreement to available evidence.
