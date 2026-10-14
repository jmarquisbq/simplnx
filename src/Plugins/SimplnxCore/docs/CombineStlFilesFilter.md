# Combine STL Files

## Group (Subgroup)

Reader/Input

## Description

This **Filter** combines all of the STL files from a given directory into a single triangle geometry. STL (STereoLithography) is a common 3D surface-mesh file format that describes a geometry as an unstructured collection of triangular facets. This filter will make use of the [Read STL File](ReadStlFileFilter.md) filter to read in each STL file in the given directory and then will proceed to combine each of the imported files into a single triangle geometry.

### File identity, coordinates and geometry limits

The input folder must already contain STL files at preflight. The filter sorts input paths and assigns positive file IDs starting at 1; row 0 of the file lookup is unused. Use the saved file-list array to interpret optional face/vertex part numbers. These IDs identify input files, not necessarily materials, grains or connected components.

Each file is read through [Read STL File](ReadStlFileFilter.md), which compacts duplicate coordinates within that file. Vertex counts and row indices can therefore change across a file round trip even when triangle coordinates are retained. The combiner appends geometries with adjusted connectivity; it does not align frames, weld across files, remove overlaps or perform a solid Boolean union.

STL does not declare coordinate units or carry arbitrary NX attributes. Establish common units and placement before aggregation, then set the native geometry's units explicitly from that provenance. Recompute normals or other derived fields when checking another source rather than assuming imported values certify a valid closed surface.

The generated part-number array can also drive [Write STL Files](WriteStlFileFilter.md) with File Grouping Type set to Part Index.

% Auto generated parameter table will be inserted here

## Example Pipelines

CombineStlFiles

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `NIST_AMBench_2018_Examples/(03) NIST Bridge STL Assembly.d3dpipeline` (SimplnxCore)

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
