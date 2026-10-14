# Generate Vector Colors

## Group (Subgroup)

Generic (Coloring)

## Description

This filter generates an RGB color for each 3-component float32 vector. The output is a 3-component uint8 array with values from 0 to 255. Each vector is normalized, so magnitude is discarded. Vectors with negative Z are reversed before coloring. Sample-plane azimuth selects a hue through red, green and blue; approaching +Z blends it toward white. This generic hemisphere encoding does not apply crystallographic symmetry or generate an IPF key. Quantization and hemisphere folding prevent exact recovery of the original vector from its color. The legend approximates the color space.

![Images/VectorColors](Images/VectorColors.png)

When *Apply to Good Voxels Only* is enabled, the supplied boolean or uint8 **Mask** array marks which **Cells** are valid; **Cells** flagged as bad are left black (RGB 0,0,0) instead of being colored.

Mask undefined, nonfinite or zero-length vectors before coloring. Retain the source vectors and their coordinate-frame convention for analysis; a direction-colored array is a display representation.

### Required Input Sources

- **Vector Attribute Array** -- a 3-component float32 per-**Cell** vector array. This may come from any filter that produces a per-**Cell** vector field (for example, a dipole, gradient, or orientation-derived vector array).

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Cambridge_Ti_CAxis_Examples/(01) Cambridge Ti Import and C-Axis Directions.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
