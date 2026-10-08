# Large data files (not in git)

These files are bigger than the repository's per-file limit for test data (Testing/README.md, "Data policy")
and live only on the machine that recorded them. The size and SHA-256 identify the original; edit the
"What" column by hand. Regenerate sizes/hashes with `python Testing/large_files.py --write <this folder>`.

| File | Size | SHA-256 | What |
|---|---|---|---|
| `data/granite_24hr.csv` | 742432461 B (742.4 MB) | `66fe94c152a0d34d1b2d245b74090f03250d653af61689f8e96bc7249e32f41c` | 24 h raw stream at ~20 Hz (displacement, phasors, error terms) -- fw 0.10.53; analysed in findings.md |
