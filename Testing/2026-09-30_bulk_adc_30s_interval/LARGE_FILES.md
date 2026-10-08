# Large data files (not in git)

These files are bigger than the repository's per-file limit for test data (Testing/README.md, "Data policy")
and live only on the machine that recorded them. The size and SHA-256 identify the original; edit the
"What" column by hand. Regenerate sizes/hashes with `python Testing/large_files.py --write <this folder>`.

| File | Size | SHA-256 | What |
|---|---|---|---|
| `analysis/phasors.npz` | 148759757 B (148.8 MB) | `693322891184c697938eae2bfae58d2f80d10f70237de03cc0f369b2f14b2e9f` | per-cycle phasors derived from the bulk captures (analysis/load.py output) |
| `analysis/quality_tune.npz` | 44754174 B (44.8 MB) | `37e56510c8085368a62885dde5eb85befcc85461c1a34f3ed58e87cf66daeb95` | intermediate arrays of the quality-indicator tuning (analysis scripts) |
| `data/bulk_adc_log.bin` | 212394240 B (212.4 MB) | `e1cb85c54d9440363dd0b54702b432b338fbb8c287779720cb8ba343d6e5a570` | the 24 h of raw ADC captures, 0.4 s every 30 s (bulk_adc_30s_test.py format) |
