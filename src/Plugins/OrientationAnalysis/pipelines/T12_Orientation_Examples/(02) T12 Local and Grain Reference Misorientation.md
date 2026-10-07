# T12 Local and Grain Reference Misorientation

Executable pipeline: `(02) T12 Local and Grain Reference Misorientation.d3dpipeline`

> **Before adapting:** This example is configured for the T12 preparation checkpoint. Review phase symmetry, validity, resolution, grain definition, and reporting population before changing inputs. Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

## At a glance

- **Input:** The full `Data/Output/T12_Orientation_Examples/Preparation/T12_Orientations.dream3d` checkpoint from [(01) T12 Orientation Preparation](%2801%29%20T12%20Orientation%20Preparation.md).
- **Result:** Two cell maps in degrees, one grain-spread array, three one-row summaries, a per-feature CSV, and a DREAM3D file with all source arrays preserved.
- **Main choice:** Compare a local, same-grain window with each cell's difference from its grain-average orientation.
- **Run order:** Run preparation first, then this pipeline from the folder containing `Data/Output`; see the [suite README](README.md). In the GUI, use **File > Open** and set absolute paths if relative paths do not resolve.

## Purpose and real-world setting

Electron backscatter diffraction (EBSD) gives an orientation at each sampled surface pixel. Local angular variation and grain-wide spread answer different questions even on the same map. [Allain-Bonasso et al. (2012), DOI 10.1016/j.msea.2012.03.068](https://doi.org/10.1016/j.msea.2012.03.068) motivates both measures for IF steel. This example uses current DREAM3D-NX filters on a public paper-linked map; it does not reproduce the paper's noise reduction, matched deformation maps, or numerical results. The archive file name does not establish its deformation state.

## Data flow and choices

| Stage | Why this choice and what to watch for |
|---|---|
| Read full checkpoint | Reuse the same measured `T12/Cell Data/Quats`, `Phases`, `FeatureIds`, `IndexedPixels`, and `T12/Cell Feature Data/AvgQuats` as preparation. The CTF import converted Euler angles from degrees to radians; the selected phase symmetry is cubic. Angular comparison uses quaternions and symmetry, not component-wise Euler subtraction or averaging. |
| Kernel Average Misorientations → `KAM_R1_Grain` | Radius **[1, 1, 0]** gives a 3 × 3 window in this single measured slice. `use_feature_ids=true` restricts comparisons to the center pixel's grain; centers with feature or phase 0 receive a placeholder zero. Preparation's grain labels keep different phases separate. The window is clipped at finite map edges. Qualifying cells are equally weighted, including the center with a zero angle and diagonal neighbors. Thus a valid one-pixel grain has zero KAM, and a zero in the map alone does not prove a flat orientation field. The paper's Equation 3 instead uses four nearest neighbors. |
| Feature Reference Misorientations → `ReferenceMisorientations`, `GrainOrientationSpread` | Mode **0** compares each cell quaternion with its grain's saved `AvgQuats`. The grain value averages those cell-reference angles over the grain. This follows the *kind* of reference used in the paper's Equations 1–2, while the exact averaging and preprocessing need not match. Mode 1 instead uses one cell farthest from a boundary and requires a distance array; it answers a different reference question. Both new angle arrays use **degrees**. No source orientation, label, size, or mask array is changed. |
| Three masked Attribute Array Statistics | `T12/PixelKAMSummary` uses `KAM_R1_Grain` and `IndexedPixels`; `T12/PixelReferenceSummary` uses `ReferenceMisorientations` and `IndexedPixels`; `T12/GrainGOSSummary` uses `GrainOrientationSpread` and `ReportableFeatures`. Each records Length, Minimum, Maximum, Mean, Median, and population StandardDeviation. The first two weight each indexed pixel once, so large grains contribute more cells. The third weights each selected grain once. The inclusive **≥4-pixel** rule applies only to this grain summary; small grains remain in the cell maps and pixel summaries. These summaries are not the paper's GAM in Equations 4–5. |
| Export | `T12_MisorientationMetrics.csv` is an ordinary comma-separated feature table with `Feature_ID` and no feature-count preamble or neighbor lists. `T12_MisorientationMetrics.dream3d` retains the complete data structure with compression level 5 and no XDMF sidecar. |

## Outputs and interpretation

Both files are written under `Data/Output/T12_Orientation_Examples/Metrics/`. The CSV contains positive feature-ID rows, including smaller grains; use its `ReportableFeatures` column to select the four-pixel grain population. It also carries the saved feature measurements, such as `NumElements`, `PixelAreas`, and `EquivalentCircleDiameters`. The DREAM3D file carries the two pixel maps and all three summary matrices.

Reference results for this prepared map:

| Summary / population | Count | Mean (°) | Population SD (°) |
| --- | ---: | ---: | ---: |
| Local KAM, all admitted pixels | 1,038,142 | 0.57397 | 0.26895 |
| Reference angle, all admitted pixels | 1,038,142 | 3.32427 | 2.03032 |
| GOS, grains with at least four pixels | 1,443 | 2.32506 | 1.31176 |

The CSV retains all 3,059 positive grain rows. Compare summary `Length` with its mask count; select `ReportableFeatures` before reproducing the grain summary from CSV. These values describe this preparation, not the paper's results.

Both maps below use a 0–10° display scale, with larger angles saturated; the stored values are unchanged. Black marks excluded pixels, while valid zeros use the low end of the color scale. The histogram includes the full GOS range for reportable grains.

![T12 KAM, grain-reference misorientation, and per-grain spread preview](T12MisorientationMetrics.png)

The 0.5 µm XY step sets the physical reach of the local radius: up to one pixel (0.5 µm) along each axis. Diagonal comparisons reach farther. Changing step size changes that reach even with the same kernel parameter. Strong map contrast can reflect boundaries, indexing noise, or real lattice rotation; these outputs alone do not quantify strain or geometrically necessary dislocation density.

For an LLM or MCP assistant: recommend this only after checking the checkpoint, phase symmetry, validity mask, spatial scale, and the desired weighting population. Treat the `.d3dpipeline` arguments as executable authority, and keep claims about deformation and paper agreement limited to evidence actually available.
