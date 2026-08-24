# Compute Volume Fractions of Ensembles

## Group (Subgroup)

Statistics (Morphological)

## Description

This **Filter** determines the volume fraction of each **Ensemble**. The **Filter** counts the number of **Cells** belonging to each **Ensemble** and stores the number fraction.

## Algorithm

Cell phase IDs are read in fixed-size sequential batches. Only one count per ensemble and one output value per ensemble are retained in memory, so working memory does not scale with the number of **Cells**. The resulting ensemble array is written with one checked bulk transfer. This same path is used for in-memory and disk-backed arrays and propagates all storage failures.

% Auto generated parameter table will be inserted here

## Example Pipelines

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
