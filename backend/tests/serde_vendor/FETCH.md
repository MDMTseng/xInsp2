# serde_vendor — bench-only third-party sources (gitignored)

`bench_record` compares cJSON vs yyjson vs MPack vs CWPack. These libs are NOT
part of the shipped backend (the production MessagePack codec uses the pinned
`backend/vendor/cwpack`). Fetch them to build the bench:

```sh
cd backend/tests/serde_vendor
mkdir -p yyjson cwpack mpack
curl -sL -o yyjson/yyjson.h https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.h
curl -sL -o yyjson/yyjson.c https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.c
curl -sL -o cwpack/cwpack.h https://raw.githubusercontent.com/clwi/CWPack/master/src/cwpack.h
curl -sL -o cwpack/cwpack_internals.h https://raw.githubusercontent.com/clwi/CWPack/master/src/cwpack_internals.h
curl -sL -o cwpack/cwpack.c https://raw.githubusercontent.com/clwi/CWPack/master/src/cwpack.c
curl -sL -o mpack.tgz https://github.com/ludocode/mpack/releases/download/v1.1.1/mpack-amalgamation-1.1.1.tar.gz
tar xzf mpack.tgz && cp mpack-amalgamation-1.1.1/src/mpack/mpack.{h,c} mpack/ && rm -rf mpack.tgz mpack-amalgamation-1.1.1
```

`cwpack/cwpack_config.h` (one line: `#define COMPILE_FOR_LITTLE_ENDIAN`) is the
only hand-written file — copy it from `backend/vendor/cwpack/cwpack_config.h`.
Then re-run cmake configure and build the `bench_record` target.
