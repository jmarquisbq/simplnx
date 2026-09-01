# Zero Crossing Image Filter

Find the pixels closest to the zero crossings (sign changes) in a signed image.

## Group (Subgroup)

ImageProcessing (ImageFeature)

## Description

Finds the pixels closest to the zero crossings (sign changes) in a **signed** scalar image. Each voxel is compared against its axial face neighbors (x±1, y±1, z±1, i.e. 4-connectivity in 2D and 6-connectivity in 3D). A voxel is labeled with the **Foreground Value** when it lies on the closer-to-zero side of a sign change with one of those neighbors; all other voxels are labeled with the **Background Value**.

The input array must be single-component (scalar) and must be a **signed** type (int8, int16, int32, int64, float32, or float64). Zero crossings are undefined for unsigned types, so unsigned input is rejected. The output is a fixed **uint8** label image regardless of the input element type. This is an ITK-free, out-of-core-capable reimplementation of the legacy ITK Zero Crossing Image Filter, and matches it exactly (byte-identical output values).

### Foreground Value

Set/Get the label value for zero-crossing pixels.

### Background Value

Set/Get the label value for non-zero-crossing pixels.

## Algorithm

For an in-memory 3D image, the filter maintains a rolling three-plane input window and writes one completed output plane at a time. When a 3D input or output is out of core, the filter first asks the shared cache budget for enough temporary memory to hold the complete input, uint8 output, and rolling planes. A complete grant lets the filter read the input once, run the same calculation against resident stores, and write the output once. A partial grant is released immediately and the filter uses the bounded rolling-plane route instead.

For a single-slice image, a checked 64 MiB plan may retain a fitting disk-backed uint8 output plane (up to 48 MiB), stream typed input row blocks through the remaining allowance, and issue one final bulk write. Larger outputs use bounded full-width row blocks or, when a complete row cannot fit, one-row X tiles with clipped halos. Every route preserves the legacy negative-axis-then-positive-axis comparison order, exact-zero behavior, and positive-direction tie rule. Datastore access remains serial and outside the parallel voxel loop.

% Auto generated parameter table will be inserted here

## Example Pipelines

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
