# Color to GrayScale

## Group (Subgroup)

Processing (Image)

## Description

This **Filter** converts 3-component uint8 RGB arrays into single-component grayscale arrays. For acquired color images, choose a conversion appropriate to the imaging method. For a colorized scientific visualization, the result is a display intensity; it does not recover the original scalar values, directions or detector signal.

### Conversion Algorithm

The *Conversion Algorithm* parameter selects how the three color channels are combined into a single value:

- **Luminosity [0]**: weighted sum of the stored RGB bytes, rounded to an integer. Configured defaults are R=0.2125, G=0.7154, B=0.0721; custom weights are supported. This operation does not perform color-profile or gamma conversion and is not a calibrated luminance measurement.
- **Average [1]**: simple arithmetic mean of the three channels: (R + G + B) / 3.
- **Lightness [2]**: average of the maximum and minimum channel values: (max(R, G, B) + min(R, G, B)) / 2.
- **SingleChannel [3]**: the user picks one channel (R, G, or B) and that channel's value is used directly.

### Additional Standard Weights

Other widely-used weighting schemes (not built-in, but easy to supply as custom Luminosity weights):

- **RMY Greyscale**: R=0.500, G=0.419, B=0.081
- **YIQ / NTSC**: R=0.299, G=0.587, B=0.114

### Input/Output Format

- **Input arrays** must be uint8 with 3 components (RGB). The current filter's selection parameter accepts three components; prepare RGB explicitly from an RGBA source before selecting it.
- **Output arrays** are single-component uint8.

Outputs are placed beside their inputs using *Output Data Array Prefix*. Select **one input array per invocation** until [issue #1759](https://github.com/BlueQuartzSoftware/simplnx/issues/1759) is fixed: with multiple selected inputs, execution overwrites the first output and leaves later outputs at their initial values. Separate consecutive invocations avoid that defect.

### Required Input Sources

- **Input Image Arrays** -- 3-component uint8 arrays from [Read Image Stack](../ImageProcessing/ReadImageStackFilter.md), another color-image reader, [Create Color Map](CreateColorMapFilter.md), or [Generate Vector Colors](ComputeVectorColorsFilter.md). Preserve the distinction between acquired image intensity and a derived display encoding.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Cambridge_Ti_CAxis_Examples/(01) Cambridge Ti Import and C-Axis Directions.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
