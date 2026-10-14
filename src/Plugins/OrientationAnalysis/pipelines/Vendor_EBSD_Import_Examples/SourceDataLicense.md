# Source provenance and acknowledgement

Both inputs are original measured vendor exports publicly distributed in the [official simplnx Data_Archive release](https://github.com/BlueQuartzSoftware/simplnx/releases/tag/Data_Archive). Archive/member identities, upload dates, byte counts and checksums are in `DataSources.json`.

- **Bruker:** `H5EspritReaderTest.h5`, scan `Section_435`, identifies nickel. The original vendor file is retained unchanged. Its source archive supplies no separate dataset licence notice.
- **Oxford:** `H5Oina_Test_Data.h5oina`, scan `1`, identifies cubic titanium. The archive contains this acknowledgement, reproduced verbatim, including its spelling:

> The DREAM3D-NX Project would like to thank Sanida National Lab for supplying the test data set.

No standard dataset license or additional redistribution permission was established for either file. Public availability is recorded separately from permission to add these files to the normal application package; that decision remains deferred until approval. This category contains pipelines, documentation and a figure of derived native outputs, not the raw vendor files.

The example converts Bruker Euler units, applies a documented Oxford sample rotation, computes phase-positive masks and descriptive summaries, and imports an additional Oxford quality field. These are example processing choices; they are not vendor recommendations or a reproduction of a research paper. The two source specimens remain independent.
