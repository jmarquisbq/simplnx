# Fuse Regular Grids (Nearest Point)

## Group (Subgroup)

Sampling (Resolution)

## Description

This filter copies numeric **Cell** data from a sampling **Image Geometry** onto a fixed reference **Image Geometry**, using their origins and spacings to map array indices. It brings separately sampled values onto one grid for comparison.

### Reference vs. Sampling Geometry

The filter works with two **Image Geometries**:

- The **Reference** geometry is the grid that is kept. Its cells are the destination.
- The **Sampling** geometry is the grid that is read from. Its cells are the source of the copied data.

For each reference index `i` on an axis, the current implementation evaluates `x = reference_origin + i * reference_spacing`. If x lies inside the sampling grid, it selects `floor((x - sampling_origin) / sampling_spacing)` and copies that cell's value. Origins are lower corners and spacings are lengths per cell. The calculation does not add a half-cell offset or perform a general nearest-center search. No interpolation or averaging is performed.

For aligned coarse-to-fine grids with an integer spacing ratio, this mapping can agree with nearest-center copying. That equivalence must be checked when changing the ratio, origins, or mapping direction. For example, a fine-to-coarse mapping can select the first fine cell in a block rather than the one nearest the coarse center. Do not alter acquisition placement merely to force the expected indexing.

The *Sampling* geometry is left unchanged. The *Reference* geometry keeps its own geometry but gains a copy of the sampling geometry's attribute arrays.

### Parameter Guidance

- **Use Custom Fill Value** — controls what happens to reference cells that fall outside the sampling grid (no nearby sampling cell). When off, copied arrays are filled with *0* there; when on, the user supplies the fill value.

The selected reference Attribute Matrix can be a separate matrix beneath the reference geometry, which avoids name collisions with retained reference arrays. Create it with tuple dimensions matching the reference grid. The source matrix must likewise correspond to the sampling grid, and the created array names must not collide with existing destination arrays.

A sampled all-one support mask becomes zero outside overlap when the fill value is zero. Use that mask for summaries; zero intensity alone cannot establish whether a cell was sampled. A nonzero custom fill also fills the mask and can invalidate this convention. Coordinate arrays copied from the source describe sampled source positions, not the destination cell centers; name them accordingly or omit them and compute reference coordinates separately.

### Required Input Sources

- **Reference Image Geometry** and **Sampling Image Geometry** -- two **Image Geometries** (each with a **Cell Attribute Matrix**), typically created by separate import or resampling steps.

## Algorithm

The filter maps each reference-grid coordinate to the containing sampling-grid **Cell** without interpolation. For in-memory arrays, the direct implementation preserves the original cell-by-cell traversal. When either a sampled numeric or Boolean **Data Array** or its newly created reference counterpart is out-of-core, the scanline implementation is selected. It calculates the source index for each X, Y, and Z coordinate once, reads only the required sampling row into a bounded buffer, and writes each completed reference row in one bulk operation. Its working memory is proportional to the largest row and the three grid axes, rather than the number of **Cells**. Only numeric and Boolean **Data Arrays** are copied; strings and neighbor lists are skipped.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.
+ `Bentheimer_Resolution_Examples/(03) Bentheimer Interior Region Comparison.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
