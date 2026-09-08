# GitLab CI

The layout follows the ESP-Claw pipeline: `.gitlab-ci.yml` provides workflow and includes, while `.gitlab/ci/` contains rules and build templates.

It builds the `esp_mosaico` firmware with ESP-IDF 6.2.

It intentionally does not configure, build, test, publish, or deploy the Mosaic simulator. The simulator remains a local development tool and is not a CI dependency.

The runner must provide the `build` tag, Docker execution, and access to the repository submodule and ESP Component Registry.
