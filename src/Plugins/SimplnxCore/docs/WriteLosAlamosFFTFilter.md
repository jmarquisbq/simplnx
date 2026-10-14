# Write Los Alamos FFT File

## Group (Subgroup)

IO (Output)

## Description

This filter writes per-**Cell** data from an **Image Geometry** to a plain-text file formatted for Ricardo Lebensohn's FFT-based simulation codes<sup>[1]</sup>. That code is a full-field micromechanical *crystal-plasticity* simulation: it predicts how a polycrystalline microstructure deforms by solving the mechanical equations on the voxel grid using a Fast Fourier Transform (FFT) method, with one material point per voxel.

Each line of the output describes a single **Cell** and combines its crystallographic orientation, its grid location, and its **Feature** (grain) and phase membership. The orientation is given as three **Euler angles** — three angles (in the Bunge Z-X-Z convention) that together specify how a crystal is rotated relative to the sample reference frame.

If possible, the filter creates any missing directories along the path to the output file.

The format of the file is an ASCII text file with the following space-delimited information:

    Phi1   Phi   Phi2  X  Y   Z  Feature_ID   Phase_ID

The **input Euler array must be in radians**. The writer converts to degrees internally and writes three decimal places; supplying degrees would convert twice. X, Y and Z are **one-based integer grid indices**, with X varying fastest. Feature ID and Phase ID are copied unchanged, so validate any positivity/indexing requirements of the receiving code rather than expecting the writer to renumber them.

The file has no header and does not carry physical origin, spacing, coordinate units, a material model, loading or boundary conditions. Retain the native geometry and its provenance when preparing the solver inputs. Exporting a cropped map does not establish that its opposite faces are suitable periodic boundaries.

### Required Input Sources

- **Image Geometry** -- the voxel grid to export.
- **Cell Euler Angles** -- a 3-component Float32 per-cell orientation array (Bunge Z-X-Z, in **radians**), typically read from EBSD data via [Read EDAX EBSD Data (.ang)](../OrientationAnalysis/ReadAngDataFilter.md), [Read Oxford Instr. EBSD Data (.ctf)](../OrientationAnalysis/ReadCtfDataFilter.md), or [Read H5EBSD File](../OrientationAnalysis/ReadH5EbsdFilter.md).
- **Cell Feature Ids** -- produced by a segmentation filter such as [Segment Features (Scalar)](ScalarSegmentFeaturesFilter.md).
- **Cell Phases** -- a per-cell phase index, read alongside the Euler angles from the EBSD readers above.

### Example Output

The output file (columns: Phi1, Phi, Phi2, X, Y, Z, Feature_ID, Phase_ID):

    90.000 0.000 0.000 1 1 1 0 1
    90.000 0.000 0.000 2 1 1 0 1
    90.000 0.000 0.000 3 1 1 0 1
    90.000 0.000 0.000 4 1 1 0 1
    135.009 55.304 295.274 18 1 1 1742 1
    90.000 0.000 0.000 19 1 1 0 1
       ..

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Gille_Aluminum_LabDCT_Examples/(01) Gille Aluminum LabDCT Preparation.d3dpipeline` (OrientationAnalysis)


## References

[1] R.A. Lebensohn, 2001. N-site modeling of a 3D viscoplastic polycrystal using Fast Fourier Transform. Acta mater. 49, 2723-2737.

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
