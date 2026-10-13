# Stage unit fixtures

The `s*/` directories contain source inputs for stage unit/integration tests,
including intentionally invalid C++ hardware descriptions and cases that are
only expected to reach a particular compiler stage. Tests refer to these paths
from the `rtlzz/` working directory.

These sources are excluded from `regression.sh` and the RTL differential sweep.
Put complete combinational regression tops under `testv2/fixtures/`, and complete
pipeline regression tops under `testv2/fixtures/pipeline_misc/`.
