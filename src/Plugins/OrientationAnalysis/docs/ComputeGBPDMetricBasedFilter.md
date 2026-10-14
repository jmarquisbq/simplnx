# Compute GBPD (Metric-Based Approach)

## Group (Subgroup)

Statistics (Crystallography)

## Description

This **Filter** computes the Grain Boundary Plane Distribution (GBPD), which describes the relative frequency of grain boundary plane orientations in a polycrystalline material, regardless of the misorientation across the boundary. While the GBCD (see [Compute GBCD](ComputeGBCDFilter.md)) describes boundaries in the full 5D space of misorientation + plane normal, the GBPD considers only the 2D distribution of boundary plane normals.

An example GBPD is shown in Fig. 1.

### Metric-Based Approach

This filter uses a metric-based approach adapted from the [Compute GBCD (Metric-Based Approach)](ComputeGBCDMetricBasedFilter.md) filter, as described by K. Glowinski and A. Morawiec in [Analysis of experimental grain boundary distributions based on boundary-space metrics, Metall. Mater. Trans. A 45, 3189-3194 (2014)](http://link.springer.com/article/10.1007%2Fs11661-014-2325-y).

The distribution is sampled at evenly distributed directions. For each sampling direction, the areas of mesh segments whose normals fall within a limiting angle &rho;<sub>p</sub> of that direction are summed. Crystal symmetry is applied so that each boundary segment is represented by up to 4 &times; *n*<sub>S</sub> equivalent vectors (where *n*<sub>S</sub> is the number of symmetry transformations). Only directions within the standard stereographic triangle need to be sampled; values at other points are obtained from symmetry.

After summing boundary areas, the distribution is normalized to multiples of a random distribution (MRD). A value of 1.0 means that plane orientation is as frequent as in a random polycrystal; values above 1.0 indicate preferred boundary plane orientations.

![Fig. 1: GBPD obtained for Small IN100 with the limiting distance set to 7&deg; and with triangles adjacent to triple lines removed. Units are MRDs.](Images/ComputeGBPDMetricBased_example.png)

The sampling model estimates relative error as **epsilon approximately 1 / sqrt(f n v)**, where *f* is density in MRD, *n* is the number of distinct grain boundaries, and *v* is the normalized neighborhood volume. Absolute error is **sqrt(f / (n v))** in MRD. The relative-error option writes **100 * absolute error / f**, capped at 100%. These estimates do not include reconstruction, segmentation, orientation-systematic, or calibration uncertainty. Use a valid unique boundary-pair table; mesh triangles and plotted directions are not independent observations.

See also the documentation for [Compute GBCD (Metric-Based Approach)](ComputeGBCDMetricBasedFilter.md) for additional information.


## Format of Output Files

The first line is `0.0 0.0 0.0 0.0` because no fixed misorientation is selected. Subsequent rows contain **azimuth degrees, elevation degrees, value**. The value is MRD for a distribution file, MRD for absolute errors, or percent for relative errors. For azimuth *a* and elevation *e*, the direction is `[cos(e) cos(a), cos(e) sin(a), sin(e)]`; convert angles to radians when using ordinary trigonometric libraries.

The writers add `_1` to a filename stem when it is absent. Requested sampling-point counts are approximate: equator points and symmetry expansion can change the number of rows. Match distribution and error rows by their angular coordinates. Do not treat an additional plotted point as an additional boundary measurement.


## Feedback

In the case of any questions, suggestions, bugs, etc., please feel free to email the author of this **Filter** at kglowinski *at* ymail.com

### Required Input Sources

This filter operates on a grain-boundary surface mesh and requires the following upstream steps:

- **Triangle Geometry** with **Face Labels**, **Face Normals**, **Face Areas**, and **Node Types** -- produced by a surface meshing filter such as [Quick Surface Mesh](../SimplnxCore/QuickSurfaceMeshFilter.md), followed by [Compute Triangle Normals](../SimplnxCore/TriangleNormalFilter.md) and [Compute Triangle Areas](../SimplnxCore/ComputeTriangleAreasFilter.md).
- **Feature Phases** -- produced by [Compute Feature Phases](../SimplnxCore/ComputeFeaturePhasesFilter.md).
- **Crystal Structures** -- ensemble-level array read from EBSD data or created by [Create Ensemble Info](CreateEnsembleInfoFilter.md).

% Auto generated parameter table will be inserted here

## References

[1] K. Glowinski and A. Morawiec, Analysis of experimental grain boundary distributions based on boundary-space metrics, Metall. Mater. Trans. A 45, 3189-3194 (2014)

## Example Pipelines

+ `Small_IN100_Distribution_Examples/(01) Small IN100 Boundary Distribution Comparison.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
