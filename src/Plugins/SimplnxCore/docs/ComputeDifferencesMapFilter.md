# Compute Differences Map

## Group (Subgroup)

Statistics (Misc)

## Description

This **Filter** calculates the difference between two **Attribute Arrays**. The arrays must have the same *primitive type* (e.g., float), *component dimensions* and *number of tuples*. The **Filter** uses the following pseudocode to calculate the difference map:

    for i to all tuples in the arrays
        for j to number of components of the arrays
            differenceMap[number of components * i + j] = firstSelectedArray[number of components * i + j] - secondSelectedArray[number of components * i + j]
        end loop over number of components
    end loop over all tuples

Note that in the above algorithm, the difference is taken as the *first selected* **Attribute Array** minus the *second selected* **Attribute Array**. The differences are also taken *component by component*. Therefore, two selected scalar arrays will result in a scalar difference map, whereas two selected 3 component vector arrays would result in a 3 component vector difference map, where component values are the scalar differences between the components of the input arrays.

**WARNING: The resulting difference values *may be negative* if the values of the first array are smaller than those in the second array. Therefore, if the two arrays are *unsigned integers*, it may be possible to underflow the resulting difference map values if the first array is sufficiently smaller than the second array. "Underflow" means that what should be a negative value will end up being a potentially very large value!**

The output Difference Map is created at the user-specified path and inherits the primitive type, component shape, and number of tuples of the two input arrays.

### Required Input Sources

None -- this filter operates on any two generic **Attribute Arrays** that share the same primitive type, component dimensions, and number of tuples, regardless of how they were produced.

% Auto generated parameter table will be inserted here

## Example Pipelines

Companion pipeline paths are relative to the named plugin's `pipelines/` directory. Each has a Markdown guide beside it.

+ `Bentheimer_Resolution_Examples/(04) Bentheimer VTK Interchange.d3dpipeline` (SimplnxCore)



+ `Bentheimer_Resolution_Examples/(02) Bentheimer Common Grid Comparison.d3dpipeline` (SimplnxCore)

+ `Small_IN100_Grain_Surface_Examples/(02) Small IN100 Grain Surface Measurements.d3dpipeline` (OrientationAnalysis)

+ `Small_IN100_Grain_Surface_Examples/(03) Small IN100 Grain Point Cloud Registration.d3dpipeline` (OrientationAnalysis)

## License & Copyright

Please see the description file distributed with this plugin.

## DREAM3D-NX Help

If you need help, need to file a bug report or want to request a new feature, please head over to the [DREAM3DNX-Issues](https://github.com/BlueQuartzSoftware/DREAM3DNX-Issues/discussions) GitHub site where the community of DREAM3D-NX users can help answer your questions.
