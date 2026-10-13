# Small IN100 Point Cloud Interchange

Executable pipeline: `(04) Small IN100 Point Cloud Interchange.d3dpipeline`

**Before adapting:** Check dataset shapes, numeric types, axis order, units, and row identities. Readable numeric arrays alone do not establish a calibrated point cloud. Validate those contracts again for another source.

## At a glance

| Item | This example |
|---|---|
| Task | Assemble numeric HDF5 point arrays into calibrated vertex geometry and a named-column CSV. |
| Prerequisite | Stage 03's saved registration file; stage 02 measurements are optional. |
| Input | Complementary TileA (89 points) and TileB (81 points) from grain 2205. |
| Output | 170 vertices in Micrometer units and six scalar CSV columns. |
| Main choices | Append A then B; preserve integer IDs; reconstruct XYZ explicitly; disable normalization. |
| Reproduction status | Illustrative interchange using two subsets of the same measured-derived grain. |

## Purpose and real-world setting

Materials workflows exchange HDF5 point arrays, then need geometry for visualization and columns for tabular analysis. This example reconstructs that contract from selected DREAM3D-NX datasets for comparison with stage 03.

Read DREAM3D is simpler for importing a complete NX DataStructure. Generic HDF5 import serves numeric datasets with a separately documented interpretation. These tiles come from one reconstruction; assembly does not test agreement between independent acquisitions.

## Input data and assumptions

Run [(03) Small IN100 Grain Point Cloud Registration](%2803%29%20Small%20IN100%20Grain%20Point%20Cloud%20Registration.md) first. Its file must exist before stage 04 preflight:

`Data/Output/Small_IN100_Grain_Surface_Examples/Registration/SmallIN100_Grain2205_PointCloudRegistration.dream3d`

Under `/DataStructure/TileA` and `/DataStructure/TileB`, import `Shared Vertex List` and three arrays under `Vertex Data`. Coordinates are float32 `[N,3]` arrays in XYZ order. `SourceROICellIndex` and `FeatureIds` are int32 `[N,1]`; `Image Quality` is float32 `[N,1]`. Here `N` is 89 or 81. The reader takes tuple and component dimensions as strings: `"89"` or `"81"`, and `"3"` or `"1"`. These dimensions describe the existing layout; changing them does not transpose axes.

`SourceROICellIndex` is the X-fastest flat index into the 24 × 24 × 24 cropped image, not a global acquisition ID. Every `FeatureIds` value is 2205. Image quality is inherited scalar metadata, not calibrated confidence. The original geometry declares Micrometer units; the numeric reader does not import that geometry metadata.

## Data flow and parameter choices

The 18 steps form five groups. Step numbers are one-based; YAML indices are zero-based.

| Steps and filters | Choice and reason |
|---|---|
| 1–4: Create Data Group; Read HDF5 Dataset | Use separate `ImportedTileA` and `ImportedTileB` groups so matching dataset leaf names cannot collide. Read four arrays per tile. |
| 5–9: Create Data Group; Concatenate Data Arrays | Append A then B for coordinates and all three scalar arrays into `AssembledTable`. Each result has 170 tuples. Types and component shapes must match within each pair. |
| 10–13: Split Data Array by Component; Rename Data Object | Split all coordinate components, retain `Coordinates_um`, and name components 0, 1, 2 as `X_um`, `Y_um`, `Z_um`. |
| 14–16: Combine Attribute Arrays; Create Geometry; Move Data | Combine XYZ in that order into `Vertices_um`, copy it into `ReassembledCloud`, explicitly set Micrometer units, and move six scalar arrays into `Vertex Data`. |
| 17–18: Write ASCII Data; Write DREAM3D | Write every point row with headers only and comma delimiters, then save geometry and inspection checkpoints. |

Concatenation appends **rows**: 89 XYZ tuples plus 81 XYZ tuples produce 170 XYZ tuples. Combining attributes joins **columns**: three scalar arrays of 170 tuples produce one three-component array. The total element count alone cannot distinguish these meanings.

Normalization stays off: scaling coordinates into `[0,1]` would destroy physical positions. Source removal stays off when combining columns because export needs scalar XYZ arrays. Integer IDs never enter that combination. Geometry construction copies the vertex array and explicitly sets the checked length unit; `_um` names alone do not assign units.

## Outputs and interpretation

Outputs use `Data/Output/Small_IN100_Grain_Surface_Examples/Interchange/`:

- `SmallIN100_Grain2205_PointCloudInterchange.dream3d` contains `ReassembledCloud`, the imported tiles, and two assembled coordinate checkpoints.
- `SmallIN100_Grain2205_PointTable.csv` contains `SourceROICellIndex,FeatureIds,X_um,Y_um,Z_um,Image Quality` in that exact order, with 170 data rows. The first point is valid and must be exported; no extra row-index column is requested.

Create the Interchange folder in the CLI working directory before running; the [category README](README.md) provides the chain. Use absolute GUI file parameters when its working directory differs.

![Complementary source tiles, reconstructed cloud, and rows read from the native CSV](SmallIN100PointCloudInterchange.png)

The preview shows native coordinates on equal micrometer axes and actual CSV rows.

## Adaptation and quality checks

For another source, change the file, dataset paths, tuple counts, and component contract together. Confirm scalar rows match coordinate rows. Separate parents prevent naming collisions; they cannot repair mismatched identity or units.

Check imported values against HDF5, A-then-B concatenation, XYZ split/recombination, moved-array placement, and geometry units. Compare final coordinates and scalar values with `CombinedTiles`, then compare by source index with `ReferenceCloud`. Require 170 unique identities and 170 CSV rows with six columns. Check IDs exactly and allow only the writer's decimal rounding for floating values. Overlapping tiles need a separate overlap policy; this example performs no deduplication.

The four-pipeline Windows in-memory Release CTest passed 1/1 in 22.76 seconds. The independent saved-output audit passed 51/51 checks, including exact reconstructed coordinates, scalar identities, Micrometer units, and all CSV rows at eight-significant-digit floating precision. Generated Python, OOC, GUI, and installed-app execution remain unverified.

## Scientific basis and references

1. [Groeber and Jackson (2014), DREAM.3D](https://doi.org/10.1186/2193-9772-3-5) provides the 3D EBSD reconstruction setting. This interchange workflow is illustrative. The article is CC BY 2.0; separate redistribution terms for the Small IN100 archive are unestablished. No measured volume is included here; archive provenance and checksum are in the category README.
2. [The HDF Group, HDF5 Datasets](https://support.hdfgroup.org/documentation/hdf5/latest/_h5_d__u_g.html) describes datasets, dimensions, datatypes, and attributes. The application must still establish what each coordinate component and unit means.

## Guidance for an LLM or MCP assistant

Treat the pipeline as executable authority and YAML as its exact argument record. Keep stage 03 as the prerequisite. Preserve A/B row order, XYZ component order, numeric types, and checked units. Describe the data as measured-derived and distinguish native checks from separately verified execution environments.
