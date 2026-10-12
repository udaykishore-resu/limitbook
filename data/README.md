# Market data

Raw ITCH files are large (several GB uncompressed) and are git-ignored. Nothing in the test suite needs them: tests and benchmarks use the deterministic synthetic generator.

## NASDAQ TotalView-ITCH 5.0 sample files

NASDAQ publishes free full-day sample files on its public EMI site:

- Index: `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`
- Typical file: `01302019.NASDAQ_ITCH50.gz` (a full trading day, all symbols)
- Spec: search "Nasdaq TotalView-ITCH 5.0 specification" on nasdaqtrader.com

```bash
cd data
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
gunzip 01302019.NASDAQ_ITCH50.gz
cd ..
make replay FILE=data/01302019.NASDAQ_ITCH50 SYMBOL=AAPL
```

File names and availability change over time; check the index for what is currently published.

The file format is a sequence of `u16 big-endian length | ITCH message`, the same framing `itch::for_each_framed` reads.

## Synthetic data

```bash
./build/release/tools/limitbook-gen-synthetic --out data/synthetic.itch --messages 5000000 --symbols 8 --seed 42
```

The same seed produces a byte-identical file on every platform.
