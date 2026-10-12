# Cambridge Ti C-Axis Grouping Comparison

Executable pipeline: `(03) Cambridge Ti C-Axis Grouping Comparison.d3dpipeline`

> **Before adapting:** **Validation warning:** Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

Run stages 01 and 02 first. This comparison shows what changes when c-axis similarity is applied directly to face-neighbor cells versus to an existing full-orientation grain graph. It is a teaching comparison on the original measured map. The paper did not propose these c-axis regions, grouped grains, or neighborhood fractions as twin or macrozone results.

## At a glance

| Item | Value |
| --- | --- |
| Input | `Data/Output/Cambridge_Ti_CAxis_Examples/GrainMetrics/CambridgeTi_GrainMetrics.dream3d` |
| Output | `Data/Output/Cambridge_Ti_CAxis_Examples/Grouping/CambridgeTi_GroupingComparison.dream3d` |
| Cell partitions | `CAxisRegionIds` and corrected `GroupedGrainIds` |
| Parent measures | `Titanium/Grouped Grain Data/Areas`, `NumCells`, `NeighborhoodAreaFraction` |
| Preview | [Boundary comparison](CambridgeTiCAxisGroupingComparison.png) on a shared native c-axis backdrop |

## Purpose and real-world setting

Projected c axes are useful for examining direction populations, but they do not retain the full orientation, including rotation about c. Holding a 7° threshold fixed exposes the effect of applying an acute c-axis angle to cells or to grains. Similar partition counts do not establish matching scientific entities or detect deformation twins.

## Data flow and filter choices

`ReadDREAM3DFilter` restores the full grain checkpoint. `CAxisSegmentFeaturesFilter` joins indexed, face-adjacent cells when their acute c-axis angle passes 7°, producing `CAxisRegionIds`. `GroupMicroTextureRegionsFilter` joins face-contiguous **grains** using c axes projected from `AvgQuats`, pairwise mode (`use_running_average=false`), no noncontiguous neighbors, fixed seed 5489, and no parent-ID randomization. This is connected-component or single-linkage behavior: a chain can yield a group whose most distant members differ by more than 7°.

The current grouping filter can give background feature row zero a positive parent. `ConditionalSetValueFilter` resets excluded cell parent labels to zero. `MultiThresholdObjectsFilter` marks feature row zero through `FeaturePhases=0`, then a second conditional replacement resets its parent label. `ComputeFeatureSizesFilter` calculates parent `NumCells` and measured `Areas` from corrected cell labels. The unused parent tuple remains in the saved attribute matrix and must be excluded by zero area and zero count. `ComputeGroupingDensityFilter` reports `NeighborhoodAreaFraction`: each populated parent's area divided by the sum of unique member-grain and immediate contiguous-neighbor areas. It is a dimensionless, additive **2D neighborhood area fraction**, not 3D density or a convex-hull packing fraction. `WriteDREAM3DFilter` saves the full comparison.

## Outputs and interpretation

The registered native Release output has 1,272 populated cell c-axis regions and 1,269 populated grouped-grain parents. The two c-axis grouping approaches agree closely on this map, differing at only three valid-neighbor edges; similar counts do not make the methods generally equivalent. The figure overlays each partition's boundaries on the same derived `acos(abs(c_x))` background and circles one of those three edges in a tight zoom. It does not recolor IDs as though their integers correspond. Both partition constructions retain excluded cells as zero. For density or size summaries, select positive parent IDs with positive `NumCells` and `Areas`; omit the empty parent row.

## Adaptation, alternatives, and pitfalls

Changing the tolerance, neighbor graph, or segmentation method can change connected components sharply. The full-orientation grain segmentation from stage 02 remains the precursor for the grain graph; substituting c-axis regions there would answer a different question. Do not call c-axis regions grains, assume all pairs within a group pass 7°, or interpret `NeighborhoodAreaFraction` as mass density, strain, or a validated macrozone score. Check background corrections, parent cell counts, area conservation, neighbor-list exclusion of background zero, and occupied-parent fractions. Repeating the fixed-seed run should repeat the partition, while numeric IDs themselves have no physical meaning.

## Scientific basis and assistant guidance

[Krakow et al. (2017), *On three-dimensional misorientation spaces*](https://doi.org/10.1098/rspa.2017.0274) uses this original alpha-Ti EBSD case in its full misorientation and twin-boundary analysis. The [Cambridge companion dataset](https://doi.org/10.17863/CAM.8815) supplies the original Oxford CPR/CRC pair and author `section4b.m` script under CC BY 4.0. Credit Robert Krakow and coauthors and the Cambridge repository when adapting or redistributing derived work. This example is **illustrative**: it does not reproduce MTEX grain identifiers, full three-parameter misorientation clusters, twin classifications, or figure-10 pixels.

For an LLM or MCP assistant: compare partitions by membership or boundary graph, not literal ID equality; separate paper outputs from NX teaching extensions. The [suite README](README.md) gives run order, source checksums, and verification scope.
