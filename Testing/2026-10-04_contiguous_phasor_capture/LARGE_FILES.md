# Large data files (not in git)

These files are bigger than the repository's per-file limit for test data (Testing/README.md, "Data policy")
and live only on the machine that recorded them. The size and SHA-256 identify the original; edit the
"What" column by hand. Regenerate sizes/hashes with `python Testing/large_files.py --write <this folder>`.

| File | Size | SHA-256 | What |
|---|---|---|---|
| `data/phasor_stream_19h_float32.npz` | 80354065 B (80.4 MB) | `084c6388bc52be0ffe53f45b0f86b1fa1e4dd9b1bf4950f9a808af469e92b869` | the 19 h recording as compact lossless float32 arrays (numpy savez_compressed); a 26000-batch excerpt is committed as tests/data/phasor_excerpt.bin |
| `data/phasor_stream_20261004_204703.csv` | 533536972 B (533.5 MB) | `063ed72dd31263152ae744b47f468ec6f37a26715ed6d6481263026cdbf08105` | the 19 h gapless phasor stream, 2 797 105 batches at 40.69/s (fw 0.10.65), original CSV |
