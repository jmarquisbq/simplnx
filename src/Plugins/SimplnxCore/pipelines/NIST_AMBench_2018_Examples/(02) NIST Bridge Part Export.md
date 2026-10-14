# NIST Bridge Part Export

Executable pipeline: `(02) NIST Bridge Part Export.d3dpipeline`

> **Before adapting:** Confirm common units and coordinate frames before exporting parts. This is a local nominal-CAD layout for file interchange, not measured specimen placement or machine instructions.

## At a glance

| Item | Contract |
| --- | --- |
| Task | Prepare four positioned part files for a batch geometry import |
| Input | Original `Data/NIST_AMBench_2018_Examples/AMB2018_01_Part.STL` |
| Prerequisite | [README setup](README.md); the contour example 01 is independent |
| Outputs | `Parts/Part01.stl` through `Part04.stl`, plus `Assembly/NISTBridge_PartLayout.dream3d`, under `Data/Output/NIST_AMBench_2018_Examples/` |
| Next | Complete this export before preflighting [(03) NIST Bridge STL Assembly](%2803%29%20NIST%20Bridge%20STL%20Assembly.md) |

## Purpose and real-world setting

An engineer may receive separate CAD meshes and need them in one consistent assembly frame for inspection or interchange. NIST's [AMB2018-01 description](https://www.nist.gov/ambench/amb2018-01-description), section 2.1.3 and Figure 3, describes four identical bridge parts with 20 mm Y spacing and 0.5 mm X staggering. This example uses those offset magnitudes in a deliberately simple local layout.

The first part stays at the source CAD origin. Subsequent copies use these translations in millimeters:

| File | X | Y | Z |
| --- | ---: | ---: | ---: |
| `Part01.stl` | 0 | 0 | 0 |
| `Part02.stl` | -0.5 | 20 | 0 |
| `Part03.stl` | -1 | 40 | 0 |
| `Part04.stl` | -1.5 | 60 | 0 |

These are example file/pose identifiers. The build plate, its absolute coordinates, materials, machine settings and measured distortions are omitted to focus on geometry transfer. A full build study needs NIST's complete plate CAD and published placement/process information, rather than treating this local layout as that model.

## Data flow and choices

| Stage | Motivation and alternatives |
| --- | --- |
| Read STL | Load the original 376-triangle binary CAD unchanged. It is a design surface, not an as-built scan. |
| Create Geometry, four copies | Copy vertices and triangles into `Part01` through `Part04` with explicit Millimeter metadata. STL has no unit declaration, and this step does not scale coordinates. |
| Apply Transformation | Translate copies 02-04 in the shared local frame before writing files. Without that positioning, importing repeated local-coordinate files would place them on top of each other. |
| Write STL Files, Single File | Write one binary STL per positioned geometry. Single File avoids interpreting engineering parts as grain/phase groups. |
| Delete temporary source; save | Keep four calibrated native geometries in the checkpoint for comparison with later imports. |

## Outputs and checks

Use an isolated `Parts` folder containing only these four generated STL files. Each has **376 triangles** and **18,884 bytes**. Open the native part-layout result and compare all four geometries: their combined bounds are **[-1.5,0,0] to [75,65,12.5] mm**. Each native copy has 194 vertex rows; STL readers may compact coincident coordinates, so those row numbers are not durable identities across file transfer.

The next example checks the binary triangle coordinates independently and preserves file identity when combining them. It must run separately because the combiner rejects an empty input directory during preflight; a cold pipeline cannot generate these files earlier in that same preflight.

## Reuse and limitations

STL transfers triangles and normals, but not NX attributes, units, original vertex numbering or pose history. Preserve the native checkpoint and [source attribution/terms](SourceDataLicense.md) when sharing derived files. The original CAD is not certified watertight, and neither export nor the subsequent aggregation performs general mesh repair, Boolean union or manufacturing validation.
