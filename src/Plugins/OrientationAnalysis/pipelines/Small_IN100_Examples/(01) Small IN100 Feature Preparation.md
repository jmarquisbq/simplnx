# Small IN100 Feature Preparation

Executable pipeline: `(01) Small IN100 Feature Preparation.d3dpipeline`

> **Before adapting:** This pipeline is configured for SmallIN100. Review its input requirements, assumptions, and parameters before using other data. Do not treat this example as a validated acceptance procedure. Adapt and validate it for the scanner, material, resolution, and quality requirement.

## At a glance

- **Input:** The archived 117-section SmallIN100 EBSD stack.
- **Result:** A reconstructed 3D volume with cleaned feature labels and periodic wrapping disabled.
- **Next:** Run Feature Measurements on the saved checkpoint.
- **Setup:** Run the existing archive pipeline first; see the [quick start](README.md#quick-start-117-section-3d-example).

## Purpose and real-world setting

Prepare a measured, finite Small IN100 EBSD volume for feature measurements. This pilot is an **illustrative** subset of the workflow in [Groeber and Jackson (2014), DOI 10.1186/2193-9772-3-5](https://doi.org/10.1186/2193-9772-3-5). It does not reproduce the paper's full method or numeric results.

## Input and run order

1. Start with the public [Small_IN100 archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz): `Data/Small_IN100/Slice_1.ang` through `Slice_117.ang`. Use installed example data or follow the quick start to extract the archive. No raw or processed volume files are included in this bundle.
2. Run the unchanged `(01) Small IN100 Archive.d3dpipeline` in `Small_IN100_Processing`. It writes `Data/Output/Reconstruction/Small_IN100.h5ebsd` using `z_spacing=0.25`.
3. Run this preparation pipeline from the same writable CLI working folder, so its relative `Data/...` paths resolve. Then run `(02) Small IN100 Feature Measurements.d3dpipeline` in this category. The [README quick start](README.md#quick-start-117-section-3d-example) gives data setup and exact Windows PowerShell commands. In the GUI, open each pipeline through **File > Open** and use absolute file input/output paths if the relative paths do not resolve for your installation.

This file is a copy of the existing `(02) Small IN100 Full Reconstruction.d3dpipeline`. Its only scientific parameter change is step 9, `EBSDSegmentFeaturesFilter.is_periodic=true` to `false`. The final output path also changes to keep the pilot checkpoint separate. The original reconstruction remains the reference. All other filter arguments, including `randomize_features=true` and twin merging, are retained. Inspect the `.d3dpipeline` for exact arguments and versions; the JSON-compatible YAML companion indexes every step.

## Data flow and choices

Steps 1–8 read the H5EBSD stack, set the quality mask, convert orientations, align sections, identify the sample, and repair bad data. Step 9 segments orientation-connected features. The finite measured sample should not join a feature across opposing volume faces, so this pilot disables periodic segmentation. Steps 10–23 compute supporting arrays, merge twins, remove small or poorly connected features, repair labels, and create IPF colors. Step 24 saves `Data/Output/Small_IN100_Examples/Preparation/SmallIN100_Features.dream3d`.

The saved measurement label is `DataContainer/Cell Data/FeatureIds`. Cell `ParentIds` were created during twin merging; later cleanup changes `FeatureIds`, and the saved parent map is absent. Do not interpret the next pipeline's results as twin-parent grain measurements.

## Expected checkpoint and annotated view

The checked pilot checkpoint had a `DataContainer` image geometry with XYZ dimensions `189 × 201 × 117`, spacing `0.25 × 0.25 × 0.25`, and saved length unit **Micrometer**. Its `Cell Feature Data` matrix held 2,335 rows and no child arrays. That empty matrix is valid: measurement filters add its arrays. It had 2,317 occupied positive feature IDs, 17 unused positive rows, and background row 0. These are observations from the local in-core Release run, not acceptance values for every rebuild.

For a result view, open the saved `DataContainer` and color the volume or slices by `Cell Data/FeatureIds` (or by `IPFColors`). Inspect the first and last Z slices together: a feature should not acquire the same ID solely through wrapping across the two faces. The Task 1 reference found one periodic label joined across those faces; this pilot split it into 813- and 880-voxel features without changing background membership. Review other faces too.

## Adaptation and checks

For another 3D scan, check raw-section order, z spacing, saved units, mask threshold, alignment, segmentation tolerance, and whether opposite faces actually represent a periodic material domain. A different geometry or acquisition can require different parameters. If an input or output file is missing, first check the predecessor stage and the chosen CLI working folder or the GUI's resolved file paths. Recheck geometry, label bounds, and feature matrix row count after any change. A fixed feature-ID shuffle seed matched logical arrays in two local runs on one binary; IDs are not guaranteed to stay the same across software versions or platforms. Compare partitions and measurements when preparation changes.

## Scientific basis and assistant guidance

The [paper](https://doi.org/10.1186/2193-9772-3-5) describes a 117-section Small IN100 reconstruction and feature-statistics workflow. The raw archive's identity with the publisher's processed supplement has not been proven. The article is CC BY 2.0; separate archive redistribution terms were not established, so the bundle includes no raw or processed volume files. Current filter behavior and parameter names come from the [simplnx source](https://github.com/BlueQuartzSoftware/simplnx). An assistant should identify the predecessor pipeline and selected `FeatureIds`, ask about spacing and physical boundaries, and present this as an illustrative method. The exact archive → preparation → measurement chain passed all six preflight/execution checks on a Windows in-core Release snapshot. The runtime copy and plugin install layout matched the source category byte for byte, with no nested category. OOC, generated Python, native GUI behavior, full application installation, and human interpretation review remain unverified.
