# Point Sample Edge Geometry

## Group (Subgroup)

Sampling (Geometry)

## Description

This filter places interior sample points along each straight edge of an **Edge Geometry**, building a new **Vertex Geometry**. For edge length L and requested spacing s, it creates `n = floor(L/s) + 1` points at fractions `(j + 0.5)/n` along the edge. Actual spacing is L/n, no greater than the request. Endpoints are excluded, and sampling restarts at each edge; it is not uniform spacing along a connected multi-edge curve.

An **Edge Geometry** is a set of straight line segments connecting pairs of vertices. In additive-manufacturing workflows each such segment is a **scan vector** — the path the energy source (e.g. a laser) travels in one pass. This filter converts those continuous segments into evenly spaced discrete points, which is useful for simulating or analyzing the path at a controlled resolution.

For every sample point the filter:

- Copies the **edge ID** of the segment the point lies on, so each point knows which original edge it came from.
- Optionally records a **cumulative sample distance** array. Its current values start at zero for the first interior sample, omitting the half-step from the edge endpoint; they do not match the documented endpoint-distance contract ([issue #1797](https://github.com/BlueQuartzSoftware/simplnx/issues/1797)).

Any additional **Edge Data Arrays** the user selects are also copied onto the matching points of the new Vertex Geometry.

### Parameter Guidance

- **Sampling Spacing (mm)** uses the raw coordinate units; the filter does not convert another input unit to millimeters. Use a positive value and explicitly calibrated millimeter input when interpreting the UI label. Zero-length edges cannot be sampled.
- **Calculate Cumulative Sample Distance** is optional. Disable it when endpoint distances are required until the discrepancy above is resolved; retained edge IDs can associate points with their source segment.
- Output geometry currently defaults to Meter metadata even for a different input unit ([issue #1796](https://github.com/BlueQuartzSoftware/simplnx/issues/1796)). Restore the source units without rescaling coordinates. Edge IDs refer to the immediate input geometry, whose row indices may differ after cropping or removal.

### Required Input Sources

- **Input Edge Geometry** -- an **Edge Geometry**, typically produced by [Slice Triangle Geometry](SliceTriangleGeometryFilter.md) or [Create AM Scan Paths](CreateAMScanPathsFilter.md).

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `NIST_AMBench_2018_Examples/(01) NIST Bridge Contour Inspection.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
