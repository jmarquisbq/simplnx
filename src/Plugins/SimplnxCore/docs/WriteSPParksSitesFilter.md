# Write SPParks Sites File

## Group (Subgroup)

IO (Output)

## Description

This filter writes a data file in the format used by [SPPARKS](http://spparks.sandia.gov/). **SPPARKS** (Stochastic Parallel PARticle Kinetic Simulator) is an open-source Kinetic Monte Carlo code from Sandia National Laboratories used to simulate microstructure evolution, such as grain growth.

SPPARKS represents the microstructure as a set of **sites**. In this export each site corresponds to one **Cell** (voxel) of the input **Image Geometry**, and the value stored at each site is that cell's **Feature** Id (the grain it belongs to). The `Values` section of the file is therefore a list of pairs: the first number in each pair is the site ID (running from *1*), and the second number is the Feature Id at that site.

The header lines describe the lattice the sites live on:

- **LINE 1** contains `-`; SPPARKS ignores this first descriptive line.
- **LINE 2** records the spatial dimension (always `3 dimension` for a 3D **Image Geometry**).
- **LINE 3** is the total number of **Cells** (sites).
- **LINE 4** is the fixed `26 max neighbors` header. It does not create or validate a neighborhood graph.
- **LINES 5, 6, and 7** give the box extents along x, y, and z. These pairs (for example `0 200`) are **cell-index** extents — dimensionless counts of cells along each axis — not physical lengths in microns.
- **LINE 8** is a blank separator before the `Values` keyword.

This writer emits **Values only**, with no `Sites` or `Neighbors` sections. Therefore, define a compatible box, sites and neighbors before reading the file, and verify that their site IDs match the export's one-based, X-fastest order. A new simulation cannot obtain its site geometry from this file alone. See the official [read_sites contract](https://spparks.github.io/doc/read_sites.html).

For a [Potts application](https://spparks.github.io/doc/app_potts.html), values must be valid spin states from 1 through Q. Grain IDs can serve as those categorical states, but this file contains no orientations, phase properties, physical scale, kinetic parameters or boundary-condition choice. A cropped measured map is not automatically periodic or a calibrated grain-growth model.

### Required Input Sources

- **Image Geometry** -- the voxel grid whose cells become SPPARKS sites.
- **Cell Feature Ids** -- produced by a segmentation filter such as [Segment Features (Scalar)](ScalarSegmentFeaturesFilter.md). The Feature Id at each cell becomes that site's value.

### Example Output

```text
[LINE 1] -
[LINE 2] 3 dimension
[LINE 3] 8000000 sites
[LINE 4] 26 max neighbors
[LINE 5] 0 200 xlo xhi
[LINE 6] 0 200 ylo yhi
[LINE 7] 0 200 zlo zhi
[LINE 8]
[LINE 9] Values
[LINE 10]
1 944
2 944
3 944
4 944
5 509
6 509
7 509
   ..
```

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Gille_Aluminum_LabDCT_Examples/(01) Gille Aluminum LabDCT Preparation.d3dpipeline` (OrientationAnalysis)


## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
