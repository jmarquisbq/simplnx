# Source provenance and reuse

Gille, Maryse; Proudhon, Henry; Oddershede, Jette; Quey, Romain; Morgeneyer, Thilo F. (2025), *LabDCT data for investigation of 3D strain heterogeneity and fracture studied by X-ray tomography and crystal plasticity in an aluminium alloy*, Materials Data Facility, [DOI 10.18126/yc1m-nk41](https://doi.org/10.18126/yc1m-nk41).

The [Argonne ACDC/MDF record](https://acdc.alcf.anl.gov/mdf/detail/0989ee07-e015-494d-8805-8e2703e76763-1.0/) identifies the dataset license as [Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/). Retain attribution, link the license and identify changes when sharing derived results. File identity and locally verified download hashes are in `DataSources.json`.

The original HDF5 is retained unchanged outside the repository. It is a published crop of measured AA6016 T4 microstructure, not the entire study volume or deformation sequence. This example imports only LabDCT, converts orientation representations and colours explicitly, recomputes NX IPF colours, adds grain statistics/flags, and exports separate voxel-orientation and grain-state text files. Its figure displays native outputs; no publication figure is copied. No raw file is uploaded or added to package installation by this change.

Associated study: Gille et al. (2024), *3D strain heterogeneity and fracture studied by X-ray tomography and crystal plasticity in an aluminium alloy*, International Journal of Plasticity 183, 104146, [DOI 10.1016/j.ijplas.2024.104146](https://doi.org/10.1016/j.ijplas.2024.104146). The preparation pipeline does not reproduce the paper's registration, DIC, Neper meshing or CP-FE predictions.
