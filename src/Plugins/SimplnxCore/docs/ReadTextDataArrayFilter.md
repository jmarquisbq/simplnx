# Read Text Data Array

## Group (Subgroup)

Core (IO/Read)

## Description

This **Filter** allows the user to import a plain text file containing the contents of a single Attribute Array. The filter does not care about how many values per line but only about reading the proper number of values from 
the file.

## Use Cases

There are several use cases for this filter. Both use cases have in common that the data
in the input file should **all** be contained in a single Data Array. If your data file
instead is organized in a table format where each column of data represents a distinct
Data Array then the [Read CSV File](ReadCSVFileFilter.md) filter should be used instead.

### Use Case 1

In this use case the user simply wants to read the data into an array within DREAM3D-NX but
does not need to store that data into an AttributeMatrix. In this case the following 
parameters should be set:

- Set Tuple Dimensions should be **ON**
- Set Data Array Dimensions to the required tuple shape. Its product is the numeric token count divided by Number of Components, not the token count when components exceed one.

### Use Case 2

In this use case the user wants to read the data into an existing Attribute Matrix. In this case
the user should set the "Created Array Path" where the parent object is the target Attribute Matrix and
also set the name of the created Data Array. The following parameters should be set
with the following values:

- Set Tuple Dimensions should be **OFF**
- DataArray Dimensions can be safely ignored as the tuple dimensions from the AttributeMatrix will be used instead.

### Example Data

The example data below has 50 scalar values. It can be read as 10 tuples with 5 components or 50 tuples with 1 component. An enclosing Attribute Matrix must have the chosen **tuple count**, excluding components: 10 tuples for the first layout or 50 for the second. No geometry is required. Confirm `tuple count × component count` against the number of numeric tokens to read; changing line breaks does not define a new array or column type.

    0    1    2    3    4    5    6    7    8    9
    10    11    12    13    14    15    16    17    18    19
    20    21    22    23    24    25    26    27    28    29
    30    31    32    33    34    35    36    37    38    39
    40    41    42    43    44    45    46    47    48    49

![Example of plain text numeric data laid out as five rows of ten values](Images/ReadTextDataArray_1.png)

### Scalar Types

| Value | Type |
|--|------|
| 0 | signed   int 8  bit |
| 1 | unsigned int 8  bit |
| 2 | signed   int 16 bit |
| 3 | unsigned int 16 bit |
| 4 | signed   int 32 bit |
| 5 | unsigned int 32 bit |
| 6 | signed   int 64 bit |
| 7 | unsigned int 64 bit |
| 8 |        Float 32 bit |
| 9 |       Double 64 bit |

### Delimiter

The *Delimiter* parameter provides the following choices:

- **, (comma) [0]**: Values are separated by a comma character.
- **; (semicolon) [1]**: Values are separated by a semicolon character.
- **  (space) [2]**: Values are separated by a space character.
- **: (colon) [3]**: Values are separated by a colon character.
- **\t (Tab) [4]**: Values are separated by a tab character.

## Required Input Sources

None — this filter reads directly from a plain text file on disk.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `T12_Import_Examples/(01) T12 Typed CSV Import.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this **Plugin**

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
