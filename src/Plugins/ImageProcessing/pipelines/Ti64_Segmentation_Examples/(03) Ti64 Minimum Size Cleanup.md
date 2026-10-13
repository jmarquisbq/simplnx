# Ti64 Minimum Size Cleanup

Executable pipeline: `(03) Ti64 Minimum Size Cleanup.d3dpipeline`

> **Before adapting:** These are connected brightness patches in one uncalibrated image. Recheck the brightness threshold, connectivity, pixel size, and 25-pixel cutoff for another image; the reported counts, selected-area fractions, and shapes do not establish grain or material-phase identity.

## At a glance

- **Input:** `Data/Output/Ti64_Segmentation_Examples/Connected/Ti64_ConnectedRegions.dream3d`, made by stage 01.
- **Result:** A report-only flag on original regions, size-cleaned labels, original/retained region counts and selected-area fractions, and retained-region shape measurements in the existing DREAM3D checkpoint and CSV.
- **Main choice:** Decide whether to filter a report or change the label image, then inspect how the selection changes region count and selected area. Use the retained shapes to compare patches with different morphology.
- **Run order:** Run cleanup 01 → 02 → 03, then segmentation 01; this stage does not depend on segmentation 02.

## Purpose and real-world setting

Connected-component labeling keeps every selected patch, including a single bright pixel. This stage compares a report-only flag with a new image that drops regions below 25 pixels to background. It then measures how much that rule changes the number of regions and their share of the crop. Shape descriptors describe the surviving patches. The cutoff is an exploratory choice, not a physical detection limit or a validated noise cutoff.

The source is a grayscale conversion, without resizing, of `Images1/image_500.png` in [Arun Baskaran's public Ti-6Al-4V repository](https://github.com/ArunBaskaran/Image-Driven-Machine-Learning-Approach-for-Microstructure-Classification-and-Segmentation-Ti-6Al-4V). The MIT notice is in [SourceDataLicense.txt](SourceDataLicense.txt); the data archive's `Provenance.json` records image transformations and hashes. [Fotos, Campbell, Murray, and Yakushina (2023)](https://doi.org/10.1007/s10853-023-08901-w) supply a microstructural-analysis setting. Their HADMA workflow uses learned class and boundary predictions followed by watershed. These native-filter examples are illustrative and neither reproduce that model nor assign grain or phase identities.

The `Ti64` image is a 596 × 596 × 1 crop of the 604 × 604 source. Its origin is [4, 4, 0] and spacing is [1, 1, 1] in uncalibrated pixels. `UnitIntensity` is a normalized, unfiltered crop. `BrightOriginal` is the preceding pipeline's 0/1 `uint8` selection using the inclusive [0.35, 1.0] interval. It is a brightness selection only. The input checkpoint also retains the source geometry, preparation, smoothing branches, and whole-crop summaries. All new arrays in this suite are added alongside those inputs.

Run the executable `.d3dpipeline` from a writable CLI folder containing `Data`. Relative inputs and outputs resolve from that folder. In the GUI, set absolute paths when `Data/...` does not resolve from the application launch directory. The matching `.yaml` lists the precise stored arguments for discovery; the pipeline is the executable authority. All three examples require the cleanup suite's preparation, smoothing, and threshold pipelines in that order. This suite then runs 01 before 02 or 03. Examples 02 and 03 each read the saved output of 01 and are independent of each other.

## Data flow and choices

| Stage | Choice, alternative, and pitfall |
|---|---|
| Read DREAM3D | Load stage 01, including original `FaceConnectedLabels`, `FaceRegionIds`, `FaceRegionData`, `BrightOriginal`, and source intensity. Preserve these baseline objects. |
| Multi-Threshold Objects, feature table | Compare `Ti64/FaceRegionData/PixelCount > 24` to create `ReportableFaceRegions`. Its true rows have at least 25 pixels; original `Feature_ID` values stay stable. It marks a report and does not remove pixels or modify `FaceConnectedLabels`. |
| Relabel Component Image | Read `FaceConnectedLabels`, set `minimum_object_size=25` and `sort_by_object_size=true`, and write `uint32` `LargeRegionLabels`. Regions under 25 pixels map to zero. Survivors get new consecutive IDs ordered by decreasing pixel count; ties keep their original label order. This operation changes an output image, not the predecessor mask. |
| Convert and mask | Copy cleaned labels to `int32` `LargeRegionIds`. Select `LargeRegionLabels > 0` into the `uint8` mask `LargeRegionMask` so removed pixels and existing background cannot contribute to indexed statistics. |
| Keep region statistics | Group `UnitIntensity` by `LargeRegionIds` under `LargeRegionMask` into `LargeRegionData`, including `PixelCount`, min/max/mean intensity, and `FeatureHasData`. This is the appropriate aggregation for varying pixel intensities. |
| Make two selection classes | Create `SelectionClassData` with two rows. Convert `BrightOriginal` and `LargeRegionMask` to `int32` classes: 0 is the unselected complement; 1 is selected. `CreateFeatureArrayFromElementArray` copies each class to its corresponding original or cleaned feature table. This copy is safe because every pixel within a region has the same class; it must not replace intensity aggregation. |
| Count regions and selected area | `ComputeNumFeatures` counts original and retained positive feature rows by class. `ComputeVolumeFractions` counts selected pixels over all 355,216 crop pixels. Although these filters name their inputs "phases," the inputs here are selection classes. On this one-slice crop the fraction measures 2D area. |
| Describe retained shapes | `ComputeFeatureRect` supplies inclusive pixel-index bounds. `ComputeMomentInvariants2D` computes normalized `Omega1`, `Omega2`, and the nine `CentralMoments` components for `LargeRegionIds`. Measure retained regions only: original labels include singleton patches, which the current moment implementation cannot safely handle. |
| Export together | The existing CSV exports every array in `LargeRegionData`, including shape descriptors, bounds, and selection class; it skips row zero. The DREAM3D file also holds both selection-class summaries and all predecessor arrays. Both filenames stay under `Cleanup/`. |

The report-only flag and relabeled image identify the same surviving pixel sets, but their IDs have different meanings. Original face ID 47, the largest region at 873 pixels, becomes cleaned ID 1. Match regions by their pixels or a computed mapping, not by numeric ID. The `ReportableFaceRegions` flag is in the original feature table; the cleaned CSV is in `LargeRegionData`. No `RequireMinimumSize` reassignment occurs here: that different operation can assign small-feature pixels to neighbors, whereas this relabel filter drops them to background zero.

## Comparison and interpretation

The two summaries in `Ti64/SelectionClassData` compare selection snapshots on the same crop:

| Measurement | Original bright regions | Retained regions (at least 25 pixels) |
|---|---:|---:|
| Positive region count | 1,166 | 266 |
| Selected pixels | 35,806 | 31,344 |
| Fraction of the full crop | 0.10080064 (10.0801%) | 0.08823927 (8.8239%) |

Removing 900 small patches removes 4,462 pixels: **77.19% of regions but 12.46% of selected area**. The retained fraction is 1.2561 percentage points lower. This shows why object count and selected area answer different questions. No surviving patch changes its pixels, so this comparison does not demonstrate shape evolution.

`OriginalRegionCount` and `RetainedRegionCount` hold `[0, 1166]` and `[0, 266]`. The count filter skips feature row zero; it does not count background as one region. `OriginalAreaFraction` and `RetainedAreaFraction` instead cover both classes and each sum to one. Their row zero is the complement, including discarded bright pixels after cleanup. These fractions do not estimate a material's 3D volume fraction.

![Original and retained bright regions, their counts and selected-area fractions, and retained shape contrasts](Ti64MinimumSizeCleanup.png)

The preview preserves original region colors in the retained map for comparison; saved cleaned IDs are size-sorted. Its shape patches use cleaned IDs and measured outputs. A removed region is small under this rule; that alone does not show it was noise.

### Read the retained shape descriptors

The moment filter measures how each patch's occupied pixel area is distributed around its centroid. It returns dimensionless, circle-normalized values. An ideal continuous circle gives `Omega1 = Omega2 = 1`. For a filled ellipse with semiaxes `a` and `b`, `Omega1 = 2ab/(a²+b²)` decreases with elongation, while `Omega2 = 1` for every aspect ratio. Thus `Omega2` alone cannot distinguish a circle from an elongated ellipse. The measured patches are irregular pixel shapes, so compare both descriptors with their label images instead of assigning a shape class from a universal cutoff.

This is useful when comparing occupied shapes without fitting an ellipse to each outline. It does not provide fitted axes, orientation, or a unique reconstruction of the outline. Pixel resolution, crop boundaries, segmentation, and the size selection all affect the result. The two normalized invariants are unchanged by ideal continuous translation, rotation, and uniform scaling; re-rasterizing an image can change its discrete shape.

`FeatureRect` stores `[xmin, ymin, zmin, xmax, ymax, zmax]` in inclusive pixel indices. `CentralMoments` stores a 3 × 3 matrix in row-major order: row is the Y exponent, column the X exponent, each from 0 to 2. Its first component is patch area in pixel²; a component of total order `p + q` has units pixel^(p+q+2). These are integrals over occupied pixel squares, not just moments of point centers. Background row zero is reserved and must be excluded from shape plots and summaries.

The mathematical method is described by [MacSleyne, Simmons, and De Graef (2008), *On the use of 2-D moment invariants for the automated classification of particle shapes*](https://doi.org/10.1016/j.actamat.2007.09.039). This example uses the implemented descriptors as an illustrative measurement; it does not reproduce that paper's microstructures or classification results.

### Checks when changing the selection

Confirm that `LargeRegionIds > 0` matches `LargeRegionMask`, that each positive cleaned ID has at least 25 pixels, and that the sum of cleaned `PixelCount` equals the selected-pixel count. The 266 true `ReportableFaceRegions` rows must identify the same surviving pixel sets despite reordered IDs. Keep each ID image paired with its own feature table. Selection classes must remain `int32` 0/1 with two summary rows and be constant inside each region. Before reusing the moment steps, check for dense positive IDs, a single XY slice, valid bounds, and finite descriptors; the supplied retained patches have no missing ID rows or singleton regions.

The 25-pixel cutoff depends on image resolution, crop, threshold, and adjacency. With unknown physical spacing, it cannot be called a minimum square-micrometer area. The result does not identify grains or reproduce the paper's learned segmentation. A defensible application would calibrate length, define the object to detect, measure false removals and retained artifacts against independent labels, and tune the cutoff on training data before checking it on held-out images. To approach Fotos et al., first reproduce their class and boundary inference and watershed with verified references.
