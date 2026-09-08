# Python/dataframe contract checks

From the terminal repository, with Python 3.13 and protoc 21.12:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r examples/requirements.txt protobuf==6.33.5
bash tests/examples/run_tests.sh
```

The runner generates Python bindings from the **terminal's own schema** into a
temporary directory and requires the wire test to execute. It checks identical
CSV/Parquet normalization, millisecond precision, explicit side and base units,
partial-minute exclusion, real WebSocket replies, candle side totals and forward
reconnect. Running unittest directly without bindings skips the wire test and
is not a complete validation. The Ubuntu build job runs the required runner.
