# Releasing BURST

Releases are automated via GitHub Actions when a version tag is pushed.

## Creating a Release

1. Ensure all changes are merged to `main` and tests pass
2. Create and push a version tag:
   ```bash
   git tag v1.0.0
   git push origin v1.0.0
   ```

## What Happens Automatically

The [release workflow](.github/workflows/release.yaml) will:

1. Build `burst-writer` and `burst-downloader` for Linux x86_64 and arm64
2. Run unit tests to verify each build
3. Package binaries into tarballs with README and LICENSE
4. Generate SHA256 checksums
5. Create a GitHub Release with auto-generated release notes

## Release Artifacts

Each release includes:

| File | Description |
|------|-------------|
| `burst-vX.Y.Z-linux-x86_64.tar.gz` | Linux x86_64 binaries |
| `burst-vX.Y.Z-linux-arm64.tar.gz` | Linux ARM64 binaries |
| `*.sha256` | Individual checksums |
| `checksums.txt` | Combined checksums file |
