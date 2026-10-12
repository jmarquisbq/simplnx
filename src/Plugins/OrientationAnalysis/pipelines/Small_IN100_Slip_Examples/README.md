# Small IN100 neighbor orientation and slip example

This folder contains one executable 3D EBSD example and its companion explanation, structured sidecar, coverage map, Python-generation specification, and a preview derived from its output. The `.d3dpipeline` is the executable authority. The [example explanation](%2801%29%20Small%20IN100%20Neighbor%20Orientation%20and%20Slip.md) describes interpretation and limits.

## Run order

The pipeline reads `Data/Output/Small_IN100_Examples/NeighborRelationships/SmallIN100_NeighborRelationships.dream3d`. Create it by running these examples in order from the same writable working directory:

1. `Small_IN100_Processing/(01) Small IN100 Archive.d3dpipeline` — import the 117 measured ANG sections.
2. `Small_IN100_Examples/(01) Small IN100 Feature Preparation.d3dpipeline` — reconstruct cleaned final feature labels.
3. `Small_IN100_Examples/(02) Small IN100 Feature Measurements.d3dpipeline` — measure final positive-ID features.
4. `Small_IN100_Examples/(06) Small IN100 Neighbor Relationships.d3dpipeline` — make final-label face-sharing contact lists.
5. `Small_IN100_Slip_Examples/(01) Small IN100 Neighbor Orientation and Slip.d3dpipeline` — calculate the orientation and slip descriptors.

The `Data/Small_IN100/` input folder must contain `Slice_1.ang` through `Slice_117.ang`. The [public archive](https://www.dream3d.io/Data_Archive/Small_IN100.tar.gz) contains these sections. The [Small IN100 examples README](../Small_IN100_Examples/README.md#quick-start-117-section-3d-example) gives extraction steps. From the chosen writable folder, create the result directory and execute:

```powershell
$work = 'C:\path\to\writable\SmallIN100Work'
$runner = 'C:\path\to\nxrunner.exe'
$examples = 'C:\path\to\pipelines' # Parent of the three named Small_IN100 folders.
Set-Location $work
New-Item -ItemType Directory -Force 'Data/Output/Small_IN100_Slip_Examples/Analysis' | Out-Null
$pipeline = Join-Path $examples 'Small_IN100_Slip_Examples/(01) Small IN100 Neighbor Orientation and Slip.d3dpipeline'
& $runner --execute $pipeline
if ($LASTEXITCODE -ne 0) { throw "Pipeline failed: $pipeline (exit $LASTEXITCODE)" }
```

Relative `Data/...` paths resolve from the CLI working directory. In the GUI, use **File > Open**; set absolute file paths if its working folder differs. The pipeline writes `Data/Output/Small_IN100_Slip_Examples/Analysis/SmallIN100_NeighborSlip.{csv,dream3d}`. The CSV has a scalar `Feature_ID` section followed by separate variable-width neighbor-list sections, so parse each section independently. The DREAM3D checkpoint keeps full positional associations. No raw ANG sections or processed volume are included here.

The 2014 [Groeber and Jackson paper](https://doi.org/10.1186/2193-9772-3-5) reports the measured Small IN100 reconstruction and orientation-statistics setting. This example is illustrative, with hypothetical sample-X/Z Schmid loadings and source-implemented fixed-Z slip descriptors. It does not recreate the publisher's numerical results or imply mechanical validation. The archive identity with the publisher's processed supplement has not been proven. `PythonGeneration.yaml` specifies temporary generation from the pipeline; its execution is a separate validation step.

The registered Windows in-core Release chain passed 1/1 in 20.38 seconds for the reference input. An independent FCC-system check covered all 28,006 directed contact entries and the 2,317 occupied feature rows; the output's scalar CSV section and seven separate neighbor-list sections were also checked. The [figure](SmallIN100NeighborSlip.png) plots 14,003 unique contact pairs and the occupied-feature Schmid distributions. These checks do not establish OOC, generated Python, GUI, installed application, or mechanical validation.
