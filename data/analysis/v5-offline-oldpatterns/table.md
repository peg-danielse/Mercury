cases: 65 (source /tmp/v3test.txt)

| metric | v3 | v4 | v5 | recorded_cpuset |
|---|---|---|---|---|
| thread_demand_mae | 0.032 | 0.038 | 0.025 | n/a |
| cores_mae | 0.162 | 0.200 | 0.137 | 0.659 |
| too_few_% | 6.068 | 5.730 | 6.082 | 11.911 |
| too_many_% | 6.838 | 10.404 | 5.033 | 29.256 |
| exact_% | 87.094 | 83.866 | 88.886 | 58.833 |
| bias | -0.008 | 0.056 | -0.000 | 0.228 |
