# Fuse Regular Grids (Nearest Point)

## Group (Subgroup)

Sampling (Resolution)

## Description

This **Filter** fuses two **Image Geometry** data sets together. The grid of **Cells** in the *Reference* **Data Container** is overlaid on the grid of **Cells** in the *Sampling* **Data Container**.  Each **Cell** in the *Reference* **Data Container** is associated with the nearest point in the *Sampling* **Data Container** (i.e., no *interpolation* is performed).  All the attributes of the **Cell** in the *Sampling* **Data Container** are then assigned to the **Cell** in the *Reference* **Data Container**.

*Note:* The *Sampling* **Data Container** remains identical after this **Filter**, but the *Reference* **Data Container**, while "geometrically identical", gains all the attribute arrays from the *Sampling* **Data Container**.

## Algorithm

The filter maps each reference-grid coordinate to the containing sampling-grid **Cell** without interpolation. For in-memory arrays, the direct implementation preserves the original cell-by-cell traversal. When either a sampled numeric or Boolean **Data Array** or its newly created reference counterpart is out-of-core, the scanline implementation is selected. It calculates the source index for each X, Y, and Z coordinate once, reads only the required sampling row into a bounded buffer, and writes each completed reference row in one bulk operation. Its working memory is proportional to the largest row and the three grid axes, rather than the number of **Cells**. Only numeric and Boolean **Data Arrays** are copied; strings and neighbor lists are skipped.

% Auto generated parameter table will be inserted here

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
