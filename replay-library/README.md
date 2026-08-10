# Replay library manifest

`manifest.json` is the source of truth for the public Replay Library widget.
The production copy is served from:

```text
https://replays.edgedepth.com/library/v1/manifest.json
```

The manifest is deliberately small and versioned. Pack objects are immutable;
the manifest may change as recordings are added or retired.

## Test another catalog

Pass an absolute HTTP or HTTPS manifest URL to the terminal:

```text
http://localhost:8000/?replayLibrary=http%3A%2F%2Flocalhost%3A9000%2Fmanifest.json
```

A host page can instead set this before loading the WebAssembly glue:

```html
<script>
  window.__EDGEDEPTH_REPLAY_LIBRARY_URL__ = "https://data.example.com/manifest.json";
</script>
```

This makes the same widget useful as a local market-data fixture browser. Pack
URLs must be absolute HTTP or HTTPS URLs. Cross-origin pack hosts must allow
`GET` with the `Range` request header and expose `Content-Range`,
`Content-Length`, and `ETag` response headers.

## Publishing rules

- Verify the `.edpack` with the repository's pack dump tooling before listing it.
- Record `size_bytes` and the SHA-256 digest in the manifest.
- Use an immutable object key for every pack revision.
- Keep descriptions factual and name the captured time window.
- Publish the manifest with `Content-Type: application/json` and a short cache
  lifetime. Pack objects can use a long immutable cache lifetime.
