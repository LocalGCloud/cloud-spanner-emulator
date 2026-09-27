<!-- generated-by: gsd-doc-writer -->
# How to Contribute

This LocalGCloud fork is not currently accepting external code contributions or pull requests.

## Issues and Feature Requests

GitHub Issues are disabled for this fork. Direct fork-specific bugs and requests
to the LocalCloud maintainers.
For behavior that also affects Google's upstream emulator, use the
[upstream issue tracker](https://github.com/GoogleCloudPlatform/cloud-spanner-emulator/issues).
For Cloud Spanner service questions, use Google's
[support channels](https://cloud.google.com/spanner/docs/getting-support).
For security issues in this fork, see [SECURITY.md](SECURITY.md).

## Maintainer Development

See [Building the Emulator](docs/building.md) for local setup and Bazel test
commands. Changes to the feature-coverage catalog or generator are checked by
[feature-coverage CI](.github/workflows/feature-coverage.yml) with
`python3 -m unittest tools.feature_coverage_test` and
`python3 tools/feature_coverage.py check`.

## Community Guidelines

This project follows
[Google's Open Source Community Guidelines](https://opensource.google.com/conduct/).
