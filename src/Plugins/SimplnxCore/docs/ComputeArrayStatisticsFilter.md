# Compute Attribute Array Statistics

## Group (Subgroup)

DREAM3D Review (Statistics)

## Description

***WARNING: The Histogram functionality has been moved to a new filter.***

This **Filter** computes a variety of statistics for a given scalar array. The currently available statistics are array length, minimum, maximum, (arithmetic) mean, median, mode, standard deviation, and summation; any combination of these statistics may be computed by this **Filter**. Any scalar array, of any primitive type, may be used as input. The type of the output arrays depends on the kind of statistic computed:

| Statistic               | Primitive Type                      |
|-------------------------|-------------------------------------|
| Length                  | unsigned 64-bit integer             |
| Minimum                 | same type as input                  |
| Maximum                 | same type as input                  |
| Mean                    | 32-bit float                        |
| Median                  | 32-bit float                        |
| Mode                    | same type as input                  |
| Standard Deviation      | 32-bit float                        |
| Summation               | 32-bit float                        |
| Standardized            | 32-bit float                        |
| Number of Unique Values | signed 32-bit integer               |

The user may optionally use a mask to specify points to be ignored when computing the statistics; only points where the supplied mask is *true* will be considered when computing statistics.  Additionally, the user may select to have the statistics computed per **Feature** or **Ensemble** by supplying an Ids array.  For example, if the user opts to compute statistics per **Feature** and selects an array that has 10 unique **Feature** Ids, then this **Filter** will compute 10 sets of statistics (e.g., find the mean of the supplied array for each **Feature**, find the total number of points in each **Feature** (the length), etc.).  

The input array may also be *standardized*, meaning that the array values will be adjusted such that they have a mean of 0 and unit variance.  This *Standardize Data* option requires the selection of both the *Find Mean* and *Find Standard Deviation* options.  The standardized data will be saved as a new 32-bit floating-point array stored in the same **Attribute Matrix** as the input array.  Note that if the *Standardize Data* option is selected, the mean and standard deviation values created by this **Filter** reflect the mean and standard deviation of the *original* array; the new standardized array has a mean of 0 and unit variance when the standard deviation is nonzero.  Constant selected data has a zero standard deviation, so its standardized values are `NaN`.  If the statistics are being computed per **Feature** or **Ensemble**, then the array values are standardized according to the mean and standard deviation *for each **Feature/Ensemble***.  For example, if 5 unique **Features** were being analyzed and *Standardize Data* was selected, then the array values for **Feature** 1 would be standardized according to the mean and standard deviation for **Feature** 1, then the array values for **Feature** 2 would be standardized according to the mean and standard deviation for **Feature** 2, and so on for the remaining **Features**.

Special operations occur for certain statistics if the supplied array is of type *bool* (for example, a mask array produced from threshold filters).  Length, minimum, maximum, median, summation, and unique-value count are supported.  Mode is restricted to integer input arrays and cannot be selected for a Boolean input.  Mean and standard deviation use the filter's historical Boolean conventions.  These operations are basic conventions and are not intended to represent Boolean logic.

### Performance

This filter is aware of out-of-core (OOC) data storage. In-memory inputs and outputs use the original direct implementation. If any enabled input, mask, numeric output, standardized output, or Mode neighbor list is out-of-core, the filter switches to a bounded scanline implementation that uses bulk reads and writes. The scanline path combines mask and Feature Id range selection while each chunk is resident and does not create a full-cell temporary mask.

Length, minimum, maximum, summation, mean, and standard deviation use bounded passes with feature-scale accumulators. Exact median, mode, and unique-value calculations use the registered external-sort capability. A bounded exact multi-pass fallback is used when no external-sort provider is available. Consequently, scanline working memory is bounded by chunk size plus enabled output/feature-scale data rather than the number of input cells.

## Destination Attribute Matrix 

The user must create a destination **Attribute Matrix** in which the computed statistics will be stored. DREAM3D-NX enforces a rule where any Attribute Matrix cannot contain another Attribute Matrix. With this in mind, the user should select a destination that is not itself an Attribute Matrix, such as the top level of a Geometry or the top level of the Data Structure itself. The user could have also created a group (using a previous filter) and use that group as the destination.

![Images/Compute_Array_Statistics_1.png](Images/Compute_Array_Statistics_1.png)
The user is creating the destination Attribute Matrix inside the `DataContainer` geometry.

![Images/Compute_Array_Statistics_2.png](Images/Compute_Array_Statistics_2.png)
The user is creating the destination Attribute Matrix inside the `Statistics` group which was created by filter #2 in the pipeline.

### Feature Range Type

The *Feature Range Type* parameter provides the following choices:

- **None [0]**: No feature-based range is applied; statistics are computed over the entire array without range constraints.
- **Ignore Feature 0 [1]**: Excludes Feature Id 0 (the invalid/background feature) from statistics calculations.
- **Shrink To Fit [2]**: Automatically determines the minimum and maximum Feature Ids present and uses that as the range, removing empty entries.
- **Padded Custom Range [3]**: Uses a user-specified range and pads with generated/filled values for Feature Ids below the minimum and above the maximum actually present in the data.
- **Minimum Size in Custom Range [4]**: Uses a user-specified range but clamps to the actual min/max Feature Ids if the specified bounds exceed them, avoiding padding.

## Ranges Breakdown

The ranges feature was added to primarily offer the following functionality:

1. option to output an array that has the Feature id in it. (Feature Ids Indexing Array)
2. option to set the "Feature Id" range.

- Allow the user to "pad out the feature ids" to a specific range
- Allow the user to only compute stats for specific feature Ids

3. option to Ignore Feature Id Zero.
4. remove empty spaces for feature ids that start above 1

All of these can be achieved with the new functionality, here's how:

### Option 1

For option 1, this array (Feature Ids Indexing Array) is automatically created for any Range selection other than `None`. The nuance here is that if your range or `Shrink To Fit` contains all the features this array will be redundant and can be removed, however, this is a very niche occurance and users are encouraged to just select `None` if they know this to be the case.

### Option 2

For option 2, this is provided with both the `Padded Custom Range` and `Minimum Size in Custom Range`. The latter is intended for users who are trying to cut down size without aproiri knowledge of the number of features. It will chop anything outside the upper bound or take the max feature if the custom upper bound exceeds it. The same is true for the lower bound in that it will take the higher of the two between provided range and the min Feature Id. `Padded Custom Range` will fill generate/fill extra values for values below and above the minimum and maximum Feature Id respectively. See the bonus section for additional range features.

### Option 3

For option 3, the ability to ignore Feature Id Zero (the invalid Feature Id) is provided directly in the form of `Ignore Feature 0` and indirectly through ranges.

### Option 4

For option 4, the most direct feature to address this is the `Shrink to Fit` range option, however it can also be achived with `Minimum Size in Custom Range`.

*Bonus: If you are unsure of the max feature id in your range, supplying a `-1` will determine the maximum feature id and use it as the upper bound in execution.*

% Auto generated parameter table will be inserted here

## Example Pipelines

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
