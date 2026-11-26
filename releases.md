# Releases

## Building packages

Build `.deb`, `.rpm`, `.tar.gz`, `.AppImage`, and a PKGBUILD for both Linux architectures
with Docker:

```sh
./yesb/build_all.py
```

Requires Docker with `buildx`. Output in `dist/`:

```
dist/
  promptr_x.y.z_amd64.deb
  promptr_x.y.z_arm64.deb
  promptr-x.y.z-1.x86_64.rpm
  promptr-x.y.z-1.aarch64.rpm
  promptr-x.y.z-amd64.AppImage
  promptr-x.y.z-arm64.AppImage
  promptr_x.y.z_amd64.tar.gz
  promptr_x.y.z_arm64.tar.gz
  SHA256SUMS
  PKGBUILD
```

## Release workflow

1. On `release/vX.Y.Z`, update `VERSION`, the AUR recipes, and `CHANGELOG.md`.
2. Build with `./yesb/build_all.py` and verify the packages and checksums.
3. Merge the release PR and push a signed `vX.Y.Z` tag on the merged commit.
4. Publish the APT, RPM, and AUR packages with their respective Yesb publishers.
5. Run `./yesb/release_direct.py --env <path-to-env-file>` to publish the packages
   and per-file checksums to the R2 `releases/` prefix. Update the download links
   in the release PR when bumping the version.

## Publishing to the apt repository

promptr is distributed via a self-hosted repository at
`https://packages.toxdes.com/apt`.

### Prerequisites

- [uv](https://docs.astral.sh/uv/) (dependencies are declared inline in the
  script)
- `gpg` for repository signing
- Docker; `[deb].helper_image` pins the Debian repository helper in `release.toml`

Package inspection and index generation run in the helper. Signing keys, GPG
prompts and uploads stay on the host; no host Debian tools are required.

Install uv:

```sh
curl -LsSf https://astral.sh/uv/install.sh | sh
```

### Environment

| Variable | Purpose |
|---|---|
| `AWS_ACCESS_KEY_ID` | Cloudflare R2 access key |
| `AWS_SECRET_ACCESS_KEY` | Cloudflare R2 secret key |
| `AWS_ENDPOINT_URL` | R2 endpoint (`https://<accountid>.r2.cloudflarestorage.com`) |
| `AWS_BUCKET` | R2 bucket name |
| `GPG_KEY_ID` | GPG key ID for repository signing |
| `GPG_PASSPHRASE` | GPG key passphrase (only needed if the key has one) |

### Publishing

Build the apt repository, sign the metadata, and upload to R2:

```sh
./yesb/release_apt.py
```

Test locally first:

```sh
./yesb/release_apt.py --dry-run    # build repo, skip upload
./yesb/release_apt.py --serve      # build and serve via HTTP (port 8080)
```

With `--serve`, test the repo in a container:

```sh
docker run --network=host --rm -it debian:bookworm bash
# Inside the container:
echo 'deb [trusted=yes] http://localhost:8080/apt stable main' \
  > /etc/apt/sources.list.d/promptr.list
apt update && apt install promptr
```

### User-facing setup (apt)

Set up the apt repository:

```sh
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://packages.toxdes.com/apt/pubkey.gpg \
  | sudo tee /etc/apt/keyrings/promptr.asc > /dev/null
sudo chmod a+r /etc/apt/keyrings/promptr.asc

sudo tee /etc/apt/sources.list.d/promptr.sources <<'EOF'
Types: deb
URIs: https://packages.toxdes.com/apt
Suites: stable
Components: main
Architectures: amd64 arm64
Signed-By: /etc/apt/keyrings/promptr.asc
EOF

sudo apt update
sudo apt install promptr
```

## Publishing to the RPM repository

promptr is distributed via a self-hosted repository at
`https://packages.toxdes.com/rpm`.

### Prerequisites

- [uv](https://docs.astral.sh/uv/)
- `gpg` for repository signing
- Docker; `[rpm].helper_image` pins the Fedora repository helper in `release.toml`

RPM signing and index tools run in the helper. Its signing adapter uses host GPG
without exporting private keys or mounting agents. No host RPM tools are required.

### Publishing

Build the RPM repository, sign the metadata, and upload to R2:

```sh
./yesb/release_rpm.py
```

Test locally first:

```sh
./yesb/release_rpm.py --dry-run    # build repo, skip upload
./yesb/release_rpm.py --serve      # build and serve via HTTP (port 8080)
```

With `--serve`, test the repo in a container:

```sh
docker run --network=host --rm -it fedora:latest bash
# Inside the container:
tee /etc/yum.repos.d/promptr.repo <<'EOF'
[promptr]
baseurl=http://localhost:8080/rpm
gpgcheck=0
enabled=1
EOF
dnf install promptr
```

### User-facing setup (rpm)

Set up the DNF repository:

```sh
sudo rpm --import https://packages.toxdes.com/rpm/pubkey.gpg

sudo tee /etc/yum.repos.d/promptr.repo <<'EOF'
[promptr]
name=Promptr
baseurl=https://packages.toxdes.com/rpm
gpgcheck=1
gpgkey=https://packages.toxdes.com/rpm/pubkey.gpg
enabled=1
EOF

sudo dnf install promptr
```

## Publishing to Arch Linux (AUR)

Pushes PKGBUILD and `.SRCINFO` to the configured stable, development and binary
AUR packages. Docker generates metadata using `[aur].srcinfo_helper_image`;
Git/SSH and any Git commit signing remain on the host.

```sh
./yesb/release_aur.py --type source  # tagged stable source
./yesb/release_aur.py --type git     # development source
./yesb/release_aur.py --type bin     # binary package
./yesb/release_aur.py --type all     # all three (default)
```

Requires an SSH key registered with your AUR account.

## Cloudflare Cache

The domain `packages.toxdes.com` serves R2 via Cloudflare with long cache TTLs.
After publishing, purge the cache so users get the latest packages:

```sh
# Check current TTL settings
./yesb/cf_purge_cache.py --check-ttl

# Set TTLs to 1 year (run once)
./yesb/cf_purge_cache.py --set-ttl

# Purge cache after publishing
./yesb/cf_purge_cache.py

# Dry-run (show what would happen)
./yesb/cf_purge_cache.py --dry-run
```

All commands load credentials from an env file:

```sh
./yesb/cf_purge_cache.py --env <path-to-env-file>
```

### Environment

| Variable | Purpose |
|---|---|
| `CF_API_TOKEN` | Cloudflare API token with `Zone.Cache Purge` + `Zone Settings` |
| `CF_ZONE_ID` | Zone ID for `toxdes.com` |
| `CF_BROWSER_TTL` | Browser cache TTL in seconds (for `--set-ttl`) |
| `CF_EDGE_TTL` | Edge cache TTL in seconds (for `--set-ttl`) |

### Token permissions

Create a **Custom API token** at https://dash.cloudflare.com/profile/api-tokens/:

| Permission | Level | Resource |
|---|---|---|
| `Zone` → `Cache Purge` | *(only action)* | `toxdes.com` |
| `Zone` → `Zone Settings` | `Edit` | `toxdes.com` |
