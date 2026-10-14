# Robust Automatic Threshold

## Group (Subgroup)

Threshold

## Description

This filter is *automatic* because it computes the threshold value itself, and *robust* because it derives that value from where the data changes most sharply rather than from a fixed cutoff. The user does not have to choose a threshold value manually; the filter selects one that tends to separate the input array along its sharpest boundaries. It produces a boolean **Mask** array that is *false* where the input array is less than the computed threshold and *true* otherwise.

The threshold is computed as a gradient-magnitude-weighted average of the input array. Each value of the input array is weighted by its corresponding gradient magnitude, so **Cells** that sit on strong boundaries (high gradient magnitude) contribute most to the chosen threshold, while flat interior regions (low gradient magnitude) contribute little. The result is a single threshold value `T` that generally partitions the input array where its gradient is highest.

The *gradient magnitude* measures how quickly the input field changes from one location to the next: it is large at edges or boundaries and near zero in smooth regions. It is obtained by computing the *2-norm* (the square root of the sum of the squares of the per-axis derivative components) of the gradient vector at each **Cell**, which collapses the multi-component gradient into a single non-negative scalar value per **Cell**. The required gradient magnitude array can be produced with the [Gradient Magnitude Image Filter](../ImageProcessing/GradientMagnitudeImageFilter.md).

### Threshold and input guidance

The cutoff is `T = sum(input * gradient) / sum(gradient)`, with **one global value for the entire array**, not a locally adaptive threshold. The mask includes values greater than or equal to T. The current filter saves the bool mask but not T. Accumulation is float32 in tuple order, so a separately computed float64 weighted mean need not produce identical mask bits near the cutoff.

Supply finite input values and finite, nonnegative gradient magnitudes from the same image and grid. The total gradient must be positive. A constant image has zero gradient and an undefined cutoff; the current implementation does not reject that case. Do not interpret an all-false mask alone as proof of a valid result. Gradient units follow the chosen image-spacing convention; the resulting cutoff has the same units as the input values.

### Required Input Sources

- **Gradient Magnitude Data** -- a single-component 32-bit float array giving the gradient magnitude of the input array, produced by [Gradient Magnitude Image Filter](../ImageProcessing/GradientMagnitudeImageFilter.md).

## Algorithm

The filter makes two sequential passes through the input **Data Array**. The first reads bounded input and gradient-magnitude batches to compute the weighted threshold in legacy tuple order. The second rereads bounded input batches and writes the boolean mask. This shared implementation works with both in-memory and out-of-core stores without per-**Cell** storage access or a cell-sized working allocation.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.
+ `Ti64_Image_Cleanup_Examples/(03) Ti64 Threshold Sensitivity.d3dpipeline` (ImageProcessing)

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
