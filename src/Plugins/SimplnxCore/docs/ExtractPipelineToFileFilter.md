# Extract DREAM3D-NX Pipeline To File

## Group (Subgroup)

IO (Output)

## Description

This **Filter** exports the pipeline embedded in a `.dream3d` HDF5 file as JSON. It selects `.json` for a legacy DREAM.3D pipeline and `.d3dpipeline` for DREAM3D-NX, correcting the requested output extension when necessary.

A `.dream3d` file *may* contain an embedded copy of the pipeline that produced it, but this is not guaranteed (for example, files written by external tools, or written with pipeline embedding disabled, will not have one). If the selected `.dream3d` file does not contain an embedded pipeline, this **Filter** has nothing to extract and will report an error rather than write an empty file.

### Provenance and replay

The source must exist before preflight. Extraction writes the stored recipe; it does not bundle raw data, upstream pipeline stages, plugin versions, or companion explanations. Relative input/output paths retain their meaning and may overwrite existing results when replayed. Review them before execution.

**Current writer limitation:** [Write DREAM3D](WriteDREAM3DFilter.md) can discard comments and re-enable disabled filters when embedding a copied pipeline ([#1807](https://github.com/BlueQuartzSoftware/simplnx/issues/1807)). The extractor faithfully exports that stored JSON and cannot recover the lost state. Keep the original pipeline and compare disabled states before replaying an extracted recipe.

### Required Input Sources

None -- the input is a `.dream3d` file selected by file path, not a **DataStructure** object created by an upstream filter.

% Auto generated parameter table will be inserted here

## Example Pipelines

+ `Bentheimer_Resolution_Examples/(04) Bentheimer VTK Interchange.d3dpipeline` (SimplnxCore)

ExtractPipelineToFile

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
