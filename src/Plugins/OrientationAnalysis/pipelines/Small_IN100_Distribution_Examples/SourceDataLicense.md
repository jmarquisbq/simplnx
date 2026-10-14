# Source attribution and redistribution status

The input is `compute_gbcd_metric_based/compute_gbcd_metric_based.dream3d` from the public [simplnx Data_Archive release](https://github.com/BlueQuartzSoftware/simplnx/releases/download/Data_Archive/compute_gbcd_metric_based.tar.gz). It is a prepared Small IN100 boundary-analysis fixture used by the existing native filter tests. The archive README describes its distribution/error outputs; the embedded pipeline records conversion from a legacy Small IN100 file.

Credit the Small IN100 data contributors and the BlueQuartz Software archive. The research article [K. Glowinski and A. Morawiec, *Analysis of Experimental Grain Boundary Distributions Based on Boundary-Space Metrics* (2014)](https://doi.org/10.1007/s11661-014-2325-y) acknowledges M. A. Groeber for permission to use Small IN100.

The article is open access under the publisher's stated Creative Commons Attribution terms. **No additional license for redistributing this processed dataset was established from the archive.** Article permission does not automatically license its input data. No raw or processed `.dream3d` input is committed or installed with this source category; package inclusion remains pending review.

Derived work here removes exactly 12 zero-area triangles, retains the other coordinates and original grain relationships, and recomputes areas/normals before native distribution calculations. The new preview renders those native outputs. The input was not reconstructed again, and no claim of exact correspondence with the paper network or absolute spatial calibration is made.
