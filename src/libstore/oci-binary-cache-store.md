R"(

**Store URL format**: `oci://`*registry*`/`*repository*

This store allows a binary cache to be stored in an OCI
(Open Container Initiative) registry, such as GitHub Container
Registry (GHCR), Docker Hub, or any self-hosted OCI-compliant
registry.

Each binary cache file is stored as an OCI blob wrapped in a
minimal OCI manifest, tagged with a sanitized version of the
file path.

For example, the following copies a package to an OCI registry:

```
# nix copy --to oci://ghcr.io/user/nix-cache nixpkgs#hello
```

And to use the registry as a substituter:

```
# nix build --extra-substituters oci://ghcr.io/user/nix-cache nixpkgs#hello
```

Authentication is handled via the OCI token authentication flow
(bearer tokens). For private registries, credentials from
`~/.docker/config.json` or the `DOCKER_CONFIG` environment
variable are used if available.

> **Note:** This store type is experimental. Enable it with
> `--extra-experimental-features oci-binary-cache-store`.

)"
