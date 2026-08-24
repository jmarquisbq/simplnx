# Remove Minimum Size Features

## Group (Subgroup)

Processing (Cleanup)

## Description

This **Filter** removes **Features** that have a total number of **Cells** below the minimum threshold defined by the user. Entering a number larger than the largest **Feature** generates an *error* (since all **Features** would be removed). Hence, a choice of threshold should be carefully chosen if it is not known how many **Cells** are in the largest **Features**. After removing all the small **Features**, the remaining **Features** are isotropically coarsened to fill the gaps left by the small **Features**.

The **Filter** can be run in a mode where the minimum number of neighbors is applied to a single **Ensemble**.  The user can select to apply the minimum to one specific **Ensemble**.

## Algorithm

### What the filter does (conceptually)

The input is a segmented volume: every **Cell** has a **Feature ID** identifying which **Feature** (grain, particle, phase domain, etc.) it belongs to. Small features — often from noise, bad data, or over-segmentation — are frequently not physically meaningful. This filter removes any feature whose voxel count is below a user-specified threshold and then reassigns those voxels to their neighbors so the volume remains fully labeled.

Running the filter leaves the volume without any sub-threshold features and with no "holes" (no voxels left unlabeled), ready for downstream analysis. Any previously-computed **feature-level** data (centroids, average orientations, neighbor lists) is invalidated by the cleanup and must be recomputed.

### Phase 1 — Mark small features as removed

The filter first walks the **per-feature voxel count** array (an array indexed by Feature ID, typically of length "thousands") and marks any feature with a count below the threshold as inactive. If "Apply Single Phase" is enabled, only features belonging to the selected phase are considered.

Then it scans the full per-cell **Feature IDs** volume in 64K-tuple chunks:

1. Bulk-read one chunk of Feature IDs into RAM (`copyIntoBuffer`).
2. For each voxel in the chunk, if its feature was marked inactive, overwrite the voxel's Feature ID with `-1` (the "bad voxel" sentinel).
3. If the chunk contained any modifications, write the chunk back (`copyFromBuffer`); otherwise skip the write.

This replaces a naive per-voxel `setValue(-1)` loop, which would issue one HDF5 chunk-op per voxel on OOC-backed data. Only chunks that actually changed incur a write.

If **every** feature would be removed by the threshold, the filter errors out — this is almost always a sign the user chose the threshold incorrectly.

### Phase 2 — Fill the gaps by majority neighbor vote

After phase 1, every voxel whose feature was too small is sitting at Feature ID `-1`. The filter iteratively relabels these bad voxels by **majority vote among their 6 face-neighbors** (±X, ±Y, ±Z):

1. **Scan pass** — Walk the volume in Z-major order using previous/current/next Feature ID slices. Each bad voxel tallies at most six IDs in a fixed local vote table; the filter does not allocate a counter proportional to the feature or cell count.
2. **Transfer pass** — Process one cell array at a time with rolling three-slice Feature ID and target-array windows. The winning neighbor tuple is copied within those local buffers and each modified target slice is written back in bulk.
3. **Feature IDs last** — Update Feature IDs only after every other cell array. This makes every array observe the same Feature ID snapshot for the iteration and preserves synchronous growth semantics without storing a cell-wide destination/source map.
4. **Iteration** — Repeat until no negative voxel has a non-negative face neighbor. Each iteration grows the surviving features by one cell layer.

### Why the rolling buffer matters

Without it, each bad-voxel evaluation would read six neighbors through per-element OOC access. With rolling slices, all neighbor decisions and tuple transfers use contiguous bulk reads and writes.

### Memory footprint

Cell arrays are processed sequentially, so peak scratch is three Feature ID slices plus three slices of the largest transferred tuple, or four Feature ID slices while Feature IDs themselves are updated. This is `O(Dx * Dy * largest_tuple_width)`. There are no cell-count-wide vectors, maps, or per-array concurrent slabs.

## WARNING: Feature Data Will Become Invalid

By modifying the cell level data, any feature data that was previously computed will most likely be invalid at this point. Filters that compute feature level data should be rerun to ensure accurate final results from your pipeline.

## WARNING: NeighborList Removal

If the Cell Feature AttributeMatrix contains any *NeighborList* data arrays, those arrays will be **REMOVED** because those lists are now invalid. Re-run the *Find Neighbors* filter to re-create the lists.

% Auto generated parameter table will be inserted here

## Example Pipelines

+ (02) Small IN100 Full Reconstruction


## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
