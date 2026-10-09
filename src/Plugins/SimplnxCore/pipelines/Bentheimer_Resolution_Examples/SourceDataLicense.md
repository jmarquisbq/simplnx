# Bentheimer source data attribution and changes

The source dataset is **Multi-resolution X-Ray micro-CT images of Bentheimer Sandstones**, by Samuel J. Jackson, Yufu Niu, Sojwal Manoorkar, Peyman Mostaghimi, and Ryan Armstrong (2021), published on Zenodo, [DOI 10.5281/zenodo.5542624](https://doi.org/10.5281/zenodo.5542624).

The dataset is licensed under [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/). The license permits sharing and adaptation with appropriate credit, a license link, and an indication of changes. This example does not imply endorsement by the dataset authors.

The scientific workflow reference is Jackson, Niu, Manoorkar, Mostaghimi, and Armstrong (2022), *Deep Learning of Multiresolution X-Ray Micro-Computed-Tomography Images for Multiscale Modeling*, **Physical Review Applied 17**, 054046, [DOI 10.1103/PhysRevApplied.17.054046](https://doi.org/10.1103/PhysRevApplied.17.054046). The dataset license applies to the data; this bundle does not redistribute the article or its figures.

## Selected source files

- `Core1_Subvol1_18micron_75cube_16bit_LE_normalised.raw`
- `Core1_Subvol1_6micron_225cube_16bit_LE_normalised.raw`

These are the complete released Core 1 / Subvolume 1 cubes. They contain scalar little-endian UInt16 intensities normalized by the authors after reconstruction and registration. Exact filenames, public download URLs, byte lengths, source MD5 values, and locally verified SHA-512 values are in [DataSources.json](DataSources.json). Raw input files are acquired separately and are not stored in this repository category.

## Changes made by these examples

Example 01 copies the unchanged source values into NX image geometries using the documented dimensions and spacings and an example-local origin. Example 02 adds fixed-divisor Float32 scaling, factor-three nearest-neighbor VLR mapping, and absolute intensity differences with scalar summaries. Example 03 changes geometry origins into a centered local frame and adds cell-center coordinates, interior masks, and masked summaries. All original intensity arrays remain available in the output checkpoints.

`Preparation.png`, `Comparison.png`, and `Regions.png` are new visualizations derived from the saved example outputs. They are not copied paper figures. Display scaling, selected middle slices, coordinate axes, ROI outlines, and annotations are visualization choices; they do not modify source arrays. The underlying data and their adaptation should retain this attribution and CC BY 4.0 license notice when shared.

The examples have illustrative reproduction status. They do not reproduce the paper's learned image enhancement or physical flow results, and they do not re-estimate the authors' normalization or registration transforms.
