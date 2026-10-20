# Authored FCC-ee display presets

Copied from `acts-dev-rd-et:/home/pagessin/dev/nodehammer/analysis/fccee/configs/`
for the four representative CI models. These were authored against Key4hep
2026-04-08 / k4geo 00-24. The original standalone TOML files are retained,
including their material definitions and provenance comments.

Each model has `simplified.toml` and `detailed.toml`. Simplified keeps silicon
sensors, physical beam-line parts, and observed subsystem envelope solids.
Detailed retains all input placements while hiding non-sensitive air/vacuum.
Display colors and envelopes are not simulation material-budget models.

CI still imports complete geometries and tests the bounded depth-2 overview.
It additionally selects and packages each authored simplified preset from the
same imported NHB. These catalog entries are labelled **package only**: the
smoke overview's triangle counts do not establish validation of the preset.
The packaging guard rejects empty selections or more than 200,000 placements.
Detailed configs are linked for download but are not automatic publications.

## IDEA detailed inputs

`IDEA_o1_v03/detailed.toml` and `IDEA_o2_v01/detailed.toml` were authored for
**reduced NHBs**, with calorimeter internals removed before configuration:

| Model | Omitted internals below | Original placements | Retained placements |
| --- | --- | ---: | ---: |
| IDEA_o1_v03 | AssemblyTube | 260,245,099 | 598,411 |
| IDEA_o2_v01 | phiER and stave_volume | 265,922,033 | 3,006,113 |

Their `tag.omitted_calorimeter_internals` rule relies on annotations added by
that reduction. These configs do not perform the reduction themselves. Applying
them to the full CI imports would retain hundreds of millions of placements.
Do not publish them against those inputs or describe the reduced scenes as full
detail. The simplified configs have explicit selection rules and were also
exercised against complete imports in the original analysis.

Historical configs for another 27 model versions remain in the remote analysis
directory; they are outside the representative CI/catalog scope.
