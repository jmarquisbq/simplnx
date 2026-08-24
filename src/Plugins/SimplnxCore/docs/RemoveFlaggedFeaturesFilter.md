# Remove/Extract Flagged Features

## Group (Subgroup)

Processing (Cleanup)

## Description

This **Filter** will remove **Features** that have been flagged by another **Filter** from the structure.  The **Filter** requires that the user point to a boolean array at the **Feature** level that tells the **Filter** whether the **Feature** should remain in the structure.  If the boolean array is *false* for a **Feature**, then all **Cells** that belong to that **Feature** are temporarily *unassigned*. Optionally, after all *undesired* **Features** are removed, the remaining **Features** are isotropically coarsened to fill in the gaps left by the removed **Features**.

### Selected Operation

The *Selected Operation* parameter provides the following choices:

- **Remove [0]**: Removes the flagged **Features** from the geometry and coarsens remaining features to fill the gaps.
- **Extract [1]**: Extracts the flagged **Features** into a new separate geometry without removing them from the original.
- **Extract then Remove [2]**: Extracts the flagged **Features** into a new geometry and then removes them from the original geometry.

## WARNING: NeighborList Removal

If the operation is [0] or [2] and the Cell Feature AttributeMatrix contains any *NeighborList* data arrays, those arrays will be **REMOVED** because those lists are now invalid. Re-run the *Find Neighbors* filter to re-create the lists.

## Caveats

This filter will **ONLY** run on an Image Geometry.

## Algorithm

### In-Core Path

`RemoveFlaggedFeaturesDirect` preserves the established in-memory workflow: it marks
cells belonging to flagged features, optionally fills those cells from the most common
face-connected neighboring feature, and compacts the feature data group.

### Out-of-Core Path

`RemoveFlaggedFeaturesScanline` is selected whenever a cell array that can be changed
by removal or filling is disk-backed. It reads and writes FeatureIds in fixed bulk
chunks. During filling it keeps only a rolling set of FeatureIds slices and one
per-slice source-mark array; each affected sibling cell array is copied with bounded
bulk slice transfers. The working memory therefore scales with an Image Geometry slice
rather than the total number of cells, while feature-level flags and compaction state
remain small resident data.

% Auto generated parameter table will be inserted here

## Example Pipelines

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
