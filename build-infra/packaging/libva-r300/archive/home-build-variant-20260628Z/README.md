# Home Build Variant

This source-only snapshot preserves the former home-level package recipe from:

```text
<home>/builds/libva-r300-wt
```

The active recipe remains `build-infra/packaging/libva-r300/PKGBUILD`. This
archived variant is older and differs in dependency/provides metadata, build
cleanup, and a temporary local `DISTCC_HOSTS` override. The active recipe is the
canonical one.

The package output and upstream tarball were not retained here because they are
reproducible from the recipe and upstream release.

`bundle_hashes.sha256` verifies the retained source files in this directory.
