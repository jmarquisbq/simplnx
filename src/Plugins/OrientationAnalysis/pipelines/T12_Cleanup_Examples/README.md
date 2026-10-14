# T12 missing-data cleanup comparison

One combined example compares three repair choices on the same deliberately damaged crop of a measured EBSD map. Read [(01) T12 Missing Data Cleanup Comparison](%2801%29%20T12%20Missing%20Data%20Cleanup%20Comparison.md) for the rationale, native preview, outcomes, and adaptation limits. The `.d3dpipeline` is authoritative; the matching YAML records its exact steps and structured context.

## Input and run order

Use the normally supplied T12 data, or extract the [public T12 archive](https://www.dream3d.io/Data_Archive/T12-MAI-2010.tar.gz) so `Data/T12-MAI-2010/fw-ar-IF1-aptr12-corr.ctf` exists under a writable working directory. Archive SHA-512:

```text
e33f224d19ad774604aa28a3263a00221a3a5909040685a3d14b6cba78e36d174b045223c28b462ab3eaea0fbc1c9f0657b1bd791a947799b9f088b13d777568
```

Run `(01) T12 Orientation Case Study` from the neighboring `T12_Orientation_Examples` category first. It applies the documented Oxford sample correction and writes `Data/Output/T12_Orientation_Examples/Preparation/T12_Orientations.dream3d`. The cleanup example reads that checkpoint. Both pipelines must use the same working directory; the orientation interchange extension is not required.

After that prerequisite, run:

```powershell
$runner = 'C:\path\to\nxrunner.exe'
$suite = 'C:\path\to\T12_Cleanup_Examples'
Set-Location 'C:\path\to\your\T12Work'
New-Item -ItemType Directory -Force 'Data/Output/T12_Cleanup_Examples' | Out-Null
& $runner --execute (Join-Path $suite '(01) T12 Missing Data Cleanup Comparison.d3dpipeline')
if ($LASTEXITCODE -ne 0) { throw 'T12 cleanup comparison failed' }
```

The result contains `Reference`, `Damaged`, and three repaired geometries, plus four CSV summaries. Source inputs and the predecessor checkpoint are retained. Faults are explicitly simulated; inferred copies are not new measurements. The native preview displays saved arrays rather than a separate implementation of the repair algorithms.

## Attribution and scope

Credit N. Allain-Bonasso, F. Wagner, S. Berbenni, and D. P. Field, [*A study of the heterogeneity of plastic deformation in IF steel by EBSD* (2012)](https://doi.org/10.1016/j.msea.2012.03.068), and the DREAM.3D public T12 archive. The archive's public availability is established by the existing T12 examples; no additional dataset license is asserted here. This is a controlled comparison using a derived crop, not reproduction of the paper's cleanup or deformation analysis.

`PythonGeneration.yaml` describes temporary conversion from the executable JSON when needed. It does not claim generated-Python, OOC, or GUI validation.
