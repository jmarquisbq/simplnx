# Cambridge Ti Import and C-Axis Directions

Executable pipeline: `(01) Cambridge Ti Import and C-Axis Directions.d3dpipeline`

> **Before adapting:** **Validation warning:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

Use this stage when you need a validity-aware, measured alpha-Ti map before any segmentation. Place the original `case_study_2_titanium_data.cpr` and `.crc` together in `Data/CambridgeTitanium/` and run from a writable working directory. The map is one XY slice of 507 by 381 pixels with 0.25 µm spacing. The importer creates unit z spacing for software geometry; it is not a measured 3D section. Preserve the raw arrays and decide your own indexed-pixel rule for another scanner or dataset.

## At a glance

| Item | Value |
| --- | --- |
| Input | `Data/CambridgeTitanium/case_study_2_titanium_data.cpr` plus paired `.crc` |
| Output | `Data/Output/Cambridge_Ti_CAxis_Examples/Prepared/CambridgeTi_Prepared.dream3d` |
| Main fields | `IndexedPixels`, unchanged `Phases`, `AnalysisPhases`, `Quats`, `CAxisDirections` |
| Measured map | 193,167 pixels; 164,274 indexed, 28,893 excluded by the stated quality rule |
| Preview | [Sample-X c-axis inclination](CambridgeTiCAxisInclination.png) derived from native `CAxisDirections` |

## Purpose and real-world setting

The paper compares spatial orientation relationships in commercially pure alpha titanium after room-temperature ballistic deformation. This preparation isolates interpretable c-axis directions in the measured specimen frame. It does not assign deformation mechanisms.

## Data flow and filter choices

`ReadChannel5DataFilter` reads both Oxford files and retains the native hexagonal crystal alignment with `edax_hexagonal_alignment=false`. `RotateSampleRefFrameFilter` applies the documented Oxford **sample** correction: 180° about sample Y. It remaps pixels without an additional crystal/Euler rotation. A basal crystal-reference change would not change projected c-axis directions, but it can change full misorientation coordinates.

`MultiThresholdObjectsFilter` marks `Error == 0 AND Bands > 0 AND Phases == 1`. The original map reports raw `Phases=1` even at unusable pixels. `CopyDataObjectFilter` and `RenameDataObjectFilter` preserve this raw array as `Phases` and create `AnalysisPhases`; `ConditionalSetValueFilter` sets only excluded analysis phases to zero. This prevents finite but unindexed orientations from entering c-axis calculations. No pixel infill or cleanup occurs. `ConvertOrientationsFilter` converts radians-based Euler angles to `Quats`, then `ComputeCAxisLocationsFilter` writes sample-frame `CAxisDirections`. `WriteDREAM3DFilter` saves the full checkpoint.

The preview plots `acos(abs(c_x))` in degrees for indexed cells from the saved native vector array. This scalar is a **derived visual quantity**, not a separate native filter output. The antipodal absolute value makes ±c equivalent. The corrected NX sample-frame view has a different left/right display convention from paper figure 10; do not transfer figure labels to this map.

## Outputs and interpretation

Indexed c-axis vectors are finite, approximately unit length, and oriented with nonnegative z by the native filter. Excluded vectors are undefined; mask them before visualization or statistics. For this source, the accepted and excluded counts above came from the native output. A small inclination means c is close to sample X, not a measured slip response or strain estimate.

## Adaptation, alternatives, and pitfalls

For a different Oxford map, inspect `Error`, `Bands`, and `Phases` distributions before copying this mask. A simple `Phases>0` rule would falsely accept 28,893 pixels here. Do not rotate Euler values by a compensating 30° to imitate a basal-axis option; this c-axis example intentionally keeps the source alignment and names that choice. A different sample-frame convention changes the visual coordinate directions and requires a documented transform. Check that raw `Phases` remains unchanged, `AnalysisPhases` equals 1 only where `IndexedPixels` is true, and undefined c-axis locations are excluded.

## Scientific basis and assistant guidance

[Krakow et al. (2017), *On three-dimensional misorientation spaces*](https://doi.org/10.1098/rspa.2017.0274) uses this original alpha-Ti EBSD case in its full misorientation and twin-boundary analysis. The [Cambridge companion dataset](https://doi.org/10.17863/CAM.8815) supplies the original Oxford CPR/CRC pair and author `section4b.m` script under CC BY 4.0. Credit Robert Krakow and coauthors and the Cambridge repository when adapting or redistributing derived work. This example is **illustrative**: it does not reproduce MTEX grain identifiers, full three-parameter misorientation clusters, twin classifications, or figure-10 pixels.

For an LLM or MCP assistant: inspect the authoritative JSON before recommending edits; keep the paired original files, relative working directory, mask semantics, and sample/crystal-frame distinction explicit. The [suite README](README.md) gives run order and provenance. Generated Python and OOC execution are separate checks.
