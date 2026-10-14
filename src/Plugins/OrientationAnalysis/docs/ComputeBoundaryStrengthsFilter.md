# Compute Feature Boundary Strength Metrics

## Group (Subgroup)

Statistics (Crystallography)

## Description

This **Filter** computes slip transmission metrics for each **Face** (triangle) on a grain boundary mesh. These dimensionless metrics describe slip-system geometry under an assumed loading direction. They do not measure boundary strength, transmitted slip, or crack initiation.

This filter computes the same metrics as the [Compute Neighbor Slip Transmission Metrics](ComputeSlipTransmissionMetricsFilter.md) filter, but stores results per boundary **Face** rather than per **Feature**. This makes it suitable for visualization and analysis on a **Triangle Geometry** surface mesh.

For a detailed explanation of the slip transmission concept and the individual metrics, see the [Compute Neighbor Slip Transmission Metrics](ComputeSlipTransmissionMetricsFilter.md) documentation.

### Material and loading scope

The cubic m-3m implementation uses the twelve FCC `{111}<110>` slip systems. Cubic symmetry alone does not make these metrics suitable for BCC slip. Supply a nonzero loading direction in the same sample reference frame as the average orientations; its magnitude is not a stress value.

Each result has two components: **Face Labels** component 0 toward component 1, and the reverse. Exterior faces receive zeros; exclude them before interpreting the metrics. All faces of a grain pair share these values because the filter uses average orientations and loading, not triangle normals.

### How This Filter Works

1. For each **Face** in the **Triangle Geometry**, the filter identifies the two **Features** on either side
2. The average orientation of both **Features** is retrieved
3. Transmission metrics are calculated in both directions (Feature 1 → Feature 2 and Feature 2 → Feature 1), since the direction of slip approaching the boundary affects the result
4. Both directional values are stored on the **Face**

### Luster-Morris Parameter (M')

The Luster-Morris parameter measures geometric compatibility between slip systems across a boundary. Calculated as presented in [1].

### Fracture Initiation Parameters (F1, F1spt, F7)

The historical F1, F1spt, and F7 formulations are discussed in [2] (page 021012-4). The current implementation selects the incoming maximum-Schmid-factor slip system. Its F1spt term uses slip-plane-normal alignment; it does not receive the boundary normal and cannot evaluate actual boundary-plane slip traces. Interpret the results as implementation-specific geometric descriptors, not calibrated fracture predictions:

![Fracture Initiation Parameter F1](Images/ComputeNeighborSlipTransmission_F1.png)

![Fracture Initiation Parameter F1spt](Images/ComputeNeighborSlipTransmission_F1spt.png)

![Fracture Initiation Parameter F7](Images/ComputeNeighborSlipTransmission_F7.png)

## References

[1] [Luster, J., Morris, M.A., 1995. *Compatibility Of Deformation In Two-Phase Ti-Al Alloys: Dependence On Microstructure And Orientation Relationships*. **Metallurgical and Materials Transactions A 26, 1745**](https://link.springer.com/article/10.1007/BF02670762)

[2] [D. Kumar, T. R. Bieler, P. Eisenlohr, D. E. Mason, M. A. Crimp, F. Roters, and D. Raabe. On Predicting Nucleation of Microcracks Due to Slip Twin Interactions at Grain Boundaries in Duplex Near γ-TiAl. Journal of Engineering Materials and Technology, 130(2):021012–12, 2008. doi:10.1115/1.2841620.](https://doi.org/10.1115/1.2841620)

### Required Input Sources

This filter operates on a grain-boundary surface mesh and requires the following upstream steps:

- **Triangle Geometry** and **Face Labels** -- produced by a surface meshing filter such as [Quick Surface Mesh](../SimplnxCore/QuickSurfaceMeshFilter.md).
- **Average Quaternions** -- the per-feature average orientation produced by [Compute Average Orientations](ComputeAvgOrientationsFilter.md).
- **Feature Phases** -- produced by [Compute Feature Phases](../SimplnxCore/ComputeFeaturePhasesFilter.md).
- **Crystal Structures** -- ensemble-level array read from EBSD data or created by [Create Ensemble Info](CreateEnsembleInfoFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
