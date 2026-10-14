# T12 Feature to Cell Mapping

Executable pipeline: `(02) T12 Feature to Cell Mapping.d3dpipeline`

> **Before adapting:** Check the feature-ID mapping and decide whether grains or pixels should have equal weight. Boundary outlines follow the segmentation's pixel resolution; copied grain values retain their grain-scale meaning.

## At a glance

- **Input:** `Data/Output/T12_Reporting_Examples/Derived/T12_ReportingFields.dream3d`, from [(01) T12 Derived Reporting Fields](%2801%29%20T12%20Derived%20Reporting%20Fields.md).
- **Result:** Three `_Cell` arrays, `T12/CellWeightedGOSSummary` and `T12BoundaryEdges`, saved in `Data/Output/T12_Reporting_Examples/Mapping/T12_FeatureMaps.dream3d`.
- **Main choice:** Broadcast grain values through `FeatureIds`, then compare a pixel-weighted mean with the equal-grain mean.
- **Run order:** Run the combined T12 Orientation Case Study, then the derived reporting-fields example first. The compact report is independent of this step; see [run commands](README.md).

## Purpose and real-world setting

Broadcasting an EBSD grain measurement to pixels makes a spatial map. A summary over that map repeats each grain's value once per pixel, giving large grains more weight. This example makes that change in the unit of observation explicit.

[Allain-Bonasso et al. (2012)](https://doi.org/10.1016/j.msea.2012.03.068) motivates grain-scale orientation analysis in IF steel. The paired weighting comparison is an illustrative NX addition using its paper-linked public T12 map. It does not reproduce the paper's figures, noise reduction, matched grains, or deformation analysis. See the [archive provenance](README.md).

## Data flow and choices

| Step | Purpose and parameter choice |
| --- | --- |
| Read the derived checkpoint | Import the full structure so the source feature values, labels, mask, and equal-grain summary remain available. |
| Map through feature IDs | `CopyFeatureArrayToElementArrayFilter` looks up `GrainOrientationSpread`, `GOSZScore`, and `ReportableFeatures` using `T12/Cell Data/FeatureIds`. Suffix `_Cell` distinguishes the three copied cell arrays from their feature sources. |
| Summarize accepted pixels | `ComputeArrayStatisticsFilter` takes `GrainOrientationSpread_Cell` and masks it with `ReportableFeatures_Cell`. Its one-row summary records the accepted cell count and angular statistics. Each accepted pixel contributes once. |
| Create Feature Boundaries (2D) | Trace shared pixel faces wherever original `FeatureIds` differ. Turn off virtual sample edges to omit the artificial outer image frame. The top Z face places the lines over the single-slice map; it adds no measured section or thickness. |
| Create Geometry; Delete Data | Retain the generated coordinates and connectivity with explicit Micrometer units, then delete the temporary geometry. No numeric scaling is applied; this addresses [unit metadata loss](https://github.com/BlueQuartzSoftware/simplnx/issues/1796). |
| Save the map | `WriteDREAM3DFilter` retains the full original structure and the copied arrays for a spatial view. |

GOS remains in degrees and z score remains dimensionless. Background ID 0 indexes feature row 0; the reporting mask excludes it from statistics. Display excluded cells separately. Their stored z-score zeros are placeholders, not evidence that their GOS equals the selected mean.

## Read the result

![Actual mapped GOS and the effect of grain versus pixel weighting](T12FeatureMaps.png)

The map uses **1,143,305 pixels** at **0.5 µm** X/Y spacing. Its 1,443 reportable grains occupy **1,036,035 pixels**. Equal grain weighting gives mean GOS **2.325059°**; pixel weighting gives **3.330604°**. Gray marks excluded pixels. The two histograms use the same eligible grains, with weights of one or each grain's pixel count. Their mean difference reflects weighting, not a change to orientations or segmentation.

### Boundary overlay

![Detail of the native mapped GOS array with and without native label-boundary edges](T12BoundaryOverlay.png)

`T12BoundaryEdges` contains **230,263 vertices and 252,527 edges**: 119,307 vertical and 133,220 horizontal pixel interfaces, each 0.5 micrometers long. Every edge matches a pair of neighboring cells with different labels. Its Z coordinate is 0, the top of this transformed single-slice geometry. The figure shows a display crop; the saved image and boundary geometry remain complete.

Outlines include interfaces with background label 0 and boundaries of grains excluded by the reporting mask. The mask controls the GOS summary and gray display cells, not boundary extraction. The image perimeter is omitted; real interfaces can end there, so the network need not form closed contours. No adjacent-grain labels or boundary misorientations are attached to the edges. Retain the source label image for ownership queries, and do not interpret this pixel staircase as a smooth physical boundary or sum its shared interfaces as independent grain perimeters.

## Adaptation and pitfalls

Check that all cells sharing an ID receive the corresponding source-row values and mask. The pixel-weighted mean should equal `sum(GOS × accepted pixel count) / sum(accepted pixel count)` over eligible grains. Verify `NumElements` against the label map before using it as that weight.

Every pixel within a grain has the same copied GOS. Use upstream reference misorientation or KAM for within-grain variation. For a feature-only report, omit these repeated cell arrays; large 3D maps can make the copies expensive. Pixel weighting corresponds to area weighting here because all cells have equal area; revisit that assumption for different geometries.

The `.d3dpipeline` is authoritative. An assistant should establish the intended unit of observation before proposing a representative mean.
