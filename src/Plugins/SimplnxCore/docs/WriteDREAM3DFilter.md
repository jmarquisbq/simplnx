# Write DREAM3D-NX File

## Group (Subgroup)

IO (Output)

## Description

This **Filter** writes the current `DataStructure` to disk as an HDF5 file with the `.dream3d` extension. The file contains every `DataArray`, geometry, attribute matrix, and group in the pipeline, along with the pipeline itself so the work can be re-run or inspected later.

### HDF5 Compression

DataArray datasets can be written with gzip (deflate) compression, dramatically reducing `.dream3d` file size at a modest CPU cost. gzip was chosen because it is non-proprietary and built into every standard HDF5 install — any tool that reads HDF5 (DREAM3D 6.x, DREAM3D-NX, HDFView, h5py, ParaView, Avizo, MATLAB) reads compressed `.dream3d` files transparently, with no special build or plugin required.

Two parameters control the behavior:

- **Use HDF5 Compression** (default: enabled). Master on/off switch. When disabled, datasets are written contiguous and uncompressed, matching the pre-compression behavior of older DREAM3D-NX builds.
- **Compression Level** (1–9, default: 5). Gzip level where 1 is fastest with the least compression and 9 is slowest with the most. Level 5 is a good balance for typical voxel and feature data; bumping to 9 usually only shrinks files a few more percent at significantly higher CPU cost.

A few details worth knowing:

- **Small arrays are written uncompressed** regardless of this setting. Arrays whose total size is under 16 KiB skip chunking/compression because the HDF5 chunk-index overhead would exceed the savings.
- **Non-DataArray metadata stays contiguous.** Only `DataArray` and `NeighborList` datasets participate in compression. Pipeline JSON, group markers, and attribute-matrix shape attributes remain unchanged — they are tiny and compressing them would slow file opens with no real benefit.
- **Chunk shape** is chosen automatically to target ~1 MiB per chunk along the tuple (outermost) dimension. This shape balances compression ratio against random-access efficiency and is not user-configurable.
- **Round-trip is lossless.** Compressed files read back byte-identical to their uncompressed equivalents.
- **File format is unchanged.** The `.dream3d` group layout and file-version tag are the same whether compression is on or off. Only the on-disk encoding of individual datasets changes, and HDF5 describes that encoding in each dataset's own metadata.

If a pipeline is imported from a legacy SIMPL (.json) file, compression is explicitly disabled so the converted pipeline preserves the exact on-disk encoding of the original SIMPL writer.

### XDMF File

When **Write Xdmf File** is enabled, the filter also produces a sidecar `.xdmf` file next to the `.dream3d` file. XDMF (eXtensible Data Model and Format) is a lightweight XML document that describes the structure of the HDF5 file in terms visualization tools understand: which datasets are vertices, which are cell data, how the mesh connectivity is laid out, and so on.

The `.xdmf` file does not contain a copy of the data — it is a metadata wrapper that points into the `.dream3d` file via HDF5 paths. The practical effect is that external visualization tools can open the `.xdmf` file and load the pipeline's geometries and attributes directly without any custom DREAM3D plugin. The primary consumers are:

- **ParaView** — open the `.xdmf` to visualize image geometries, triangle meshes, vertex clouds, and any DataArrays attached to them.
- **VisIt** — same workflow, same capabilities.

Enabling XDMF costs almost nothing (the file is small and fast to write) and is the recommended setting whenever visualization outside DREAM3D-NX is a possibility. It can safely be left off for intermediate pipeline outputs that will only be re-read by DREAM3D-NX itself.

### Required Input Sources

None — this filter writes whatever currently exists in the **DataStructure** to the output file. If the file already exists it is overwritten.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Bentheimer_Resolution_Examples/(04) Bentheimer VTK Interchange.d3dpipeline` (SimplnxCore)

+ `T12_Orientation_Examples/(02) T12 Orientation Interchange.d3dpipeline` (OrientationAnalysis)



+ `(05) AM XCT Porosity Segmentation/(05) AM XCT Porosity Segmentation.d3dpipeline` (ImageProcessing)
+ `(06) AM Powder-Bed Layer Inspection/(06) AM Powder-Bed Layer Inspection.d3dpipeline` (ImageProcessing)
+ `(07) AM Melt-Pool and Track Inspection/(07) AM Melt-Pool and Track Inspection.d3dpipeline` (ImageProcessing)
+ `(08) Powder Particle Watershed Segmentation/(08) Powder Particle Watershed Segmentation.d3dpipeline` (ImageProcessing)
+ `(09) Microstructure Watershed Segmentation/(09) Microstructure Watershed Segmentation.d3dpipeline` (ImageProcessing)
+ `(10) Binary Mask Repair and Skeletonization/(10) Binary Mask Repair and Skeletonization.d3dpipeline` (ImageProcessing)
+ `(11) Grayscale Surface-Defect Morphology/(11) Grayscale Surface-Defect Morphology.d3dpipeline` (ImageProcessing)
+ `(12) Pore Distance and Wall-Thickness Metrology/(12) Pore Distance and Wall-Thickness Metrology.d3dpipeline` (ImageProcessing)
+ `(13) Denoising and Edge Detection/(13) Denoising and Edge Detection.d3dpipeline` (ImageProcessing)
+ `(14) Projection-Based Quality Summaries/(14) Projection-Based Quality Summaries.d3dpipeline` (ImageProcessing)
+ `(15) Radiography Intensity Calibration/(15) Radiography Intensity Calibration.d3dpipeline` (ImageProcessing)
+ `(16) Phase and Angle Field Transforms/(16) Phase and Angle Field Transforms.d3dpipeline` (ImageProcessing)
+ `(17) Scientific Volume Interoperability/(17) Scientific Volume Interoperability.d3dpipeline` (ImageProcessing)
+ `(18) Industrial XCT Format Import/(18) Industrial XCT Format Import.d3dpipeline` (ImageProcessing)
+ `(19) Serial-Section Stack Reconstruction/(19) Serial-Section Stack Reconstruction.d3dpipeline` (ImageProcessing)
+ `(20) Fiji Microscopy Montage Import/(20) Fiji Microscopy Montage Import.d3dpipeline` (ImageProcessing)
+ `Ti64_Image_Cleanup_Examples/(01) Ti64 Image Preparation.d3dpipeline` (ImageProcessing)
+ `Ti64_Image_Cleanup_Examples/(02) Ti64 Smoothing Comparison.d3dpipeline` (ImageProcessing)
+ `Ti64_Image_Cleanup_Examples/(03) Ti64 Threshold Sensitivity.d3dpipeline` (ImageProcessing)
+ `Ti64_Segmentation_Examples/(01) Ti64 Connected Bright Regions.d3dpipeline` (ImageProcessing)
+ `Ti64_Segmentation_Examples/(02) Ti64 Connectivity Comparison.d3dpipeline` (ImageProcessing)
+ `Ti64_Segmentation_Examples/(03) Ti64 Minimum Size Cleanup.d3dpipeline` (ImageProcessing)
+ `Cambridge_Ti_CAxis_Examples/(01) Cambridge Ti Import and C-Axis Directions.d3dpipeline` (OrientationAnalysis)
+ `Cambridge_Ti_CAxis_Examples/(02) Cambridge Ti Grain C-Axis Metrics.d3dpipeline` (OrientationAnalysis)
+ `Cambridge_Ti_CAxis_Examples/(03) Cambridge Ti C-Axis Grouping Comparison.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Boundary_Examples/(01) Small IN100 Boundary and Section Measurements.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(02) Small IN100 Feature Measurements.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(03) Small IN100 Surface Selection Comparison.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(04) Small IN100 Minimum Size Comparison.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(05) Small IN100 Combined Selection.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(06) Small IN100 Neighbor Relationships.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Examples/(07) Small IN100 Bounded Voxel Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(01) Small IN100 Surface Mesh.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(02) Small IN100 Surface Smoothing.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Geometry_Examples/(03) Small IN100 Surface Transform and Export.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(01) Small IN100 Complete Grain Surface.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Slip_Examples/(01) Small IN100 Neighbor Orientation and Slip.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Spatial_Examples/(01) Small IN100 Feature Bounds.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Spatial_Examples/(02) Small IN100 Feature and Box Quality.d3dpipeline` (OrientationAnalysis)
+ `T12_Import_Examples/(01) T12 Typed CSV Import.d3dpipeline` (OrientationAnalysis)
+ `T12_Import_Examples/(02) T12 Selective DREAM3D Import.d3dpipeline` (OrientationAnalysis)
+ `T12_Orientation_Examples/(01) T12 Orientation Case Study.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(01) T12 Derived Reporting Fields.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(02) T12 Feature to Cell Mapping.d3dpipeline` (OrientationAnalysis)
+ `T12_Reporting_Examples/(03) T12 Compact Report.d3dpipeline` (OrientationAnalysis)
+ `Bentheimer_Resolution_Examples/(01) Bentheimer Native Volume Preparation.d3dpipeline` (SimplnxCore)
+ `Bentheimer_Resolution_Examples/(02) Bentheimer Common Grid Comparison.d3dpipeline` (SimplnxCore)
+ `Bentheimer_Resolution_Examples/(03) Bentheimer Interior Region Comparison.d3dpipeline` (SimplnxCore)

+ `Small_IN100_Examples/(08) Small IN100 Grain Size Groups and Representatives.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(03) Small IN100 Grain Point Cloud Registration.d3dpipeline` (OrientationAnalysis)
+ `Small_IN100_Grain_Surface_Examples/(04) Small IN100 Point Cloud Interchange.d3dpipeline` (OrientationAnalysis)

+ `Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline` (OrientationAnalysis)

+ `OIM_Nickel_Pattern_Examples/(01) OIM Nickel Pattern Inspection.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
