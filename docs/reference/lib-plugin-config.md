# Lib-plugin deployment config (`--lib-config`)

A **lib plugin** (e.g. `imgcodec`) can be autoloaded once at service boot as a
machine provider (`--autoload-lib`), WITHOUT any project declaring a per-instance.
Such an autoloaded provider previously ran only on its **compiled-in defaults** —
the `plugin.json` `manifest.params` defaults are applied only when a *project*
instance declares them. `--lib-config` closes that gap: a **per-machine deployment
config** applied to each autoloaded provider's `set_def` before it serves.

## Usage

```
xinsp-backend --autoload-lib --lib-config C:\deploy\lib-config.json
# or: set XINSP2_LIB_CONFIG=C:\deploy\lib-config.json   (CLI flag wins over env)
```

The file is a JSON **object** mapping plugin name → its def object (the same shape
the plugin's `set_def` accepts / `get_def` returns):

```json
{
  "imgcodec": {
    "quality": 90,
    "encode_max_concurrent": 6
  }
}
```

At boot the service reads it, and for each autoloaded machine provider whose name is
a key, applies that object via `set_def`. Boot log:

```
[xinsp2] --lib-config: 1 entry(s) from C:\deploy\lib-config.json
[xinsp2] autoload: applied --lib-config to 'imgcodec'
```

## Semantics

- **Deployment-level, per machine.** The file lives wherever the deployment puts it
  (NOT in the plugin package), so it survives plugin rebuilds and can differ per box
  (e.g. tune `encode_max_concurrent` to the machine's core count).
- **Only autoloaded machine providers.** A **project instance** of the same plugin
  still wins — it displaces the machine provider with its own config for the life of
  the project, and is reinstated (with the deployment config) on project close.
- **Non-fatal.** Missing file, non-JSON, or a `set_def`-rejected entry leaves the
  compiled defaults and logs a warning; boot proceeds.
- **Gated by `--autoload-lib`.** With autoload off there is no machine provider to
  configure, so `--lib-config` is only read when autoload is enabled.
- Unknown plugin names in the file are simply never matched (harmless).

## imgcodec config keys (the current lib plugin)

| key | default | meaning |
|-----|---------|---------|
| `quality` | 85 | default JPEG quality 1..100 (per-request `quality` still overrides) |
| `cache_max` | 32 | dedup memo cache capacity (FIFO) |
| `encode_max_concurrent` | 0 | cap on simultaneous JPEG encodes (0 = unlimited). Bounds CPU so encoding can't starve inspection; gates only real encodes, never a dedup cache hit. A single encode is single-threaded (libjpeg-turbo) — this limits parallelism across concurrent requests, not one image. |
```
