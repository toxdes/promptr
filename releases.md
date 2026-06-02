# Releases

## Building packages

Build `.deb`, `.rpm`, `.AppImage`, and a PKGBUILD for all platforms
with Docker:

```sh
./build-all.py
```

For AppImages:

```sh
./build-all.py --include-appimage
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
  PKGBUILD
```

## Release workflow

1. Bump the version in `VERSION`, commit
2. Tag: `git tag v0.1.7 && git push origin v0.1.7`
3. Build: `./build-all.py [--include-appimage]`. Skip AppImage for
   smaller releases.
4. Create a GitHub release and upload artifacts from `dist/`
5. Push to AUR: `./release-aur.py`
6. Push to apt repository: `./release-apt.py`
7. Push to RPM repository: `./release-rpm.py`

## Publishing to the apt repository

promptr is distributed via a self-hosted repository at
`https://packages.toxdes.com/apt`.

### Prerequisites

- [uv](https://docs.astral.sh/uv/) (dependencies are declared inline in the
  script)
- `gpg` for repository signing
- `dpkg-deb` for package inspection (usually already installed on
  Debian/Ubuntu)

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
./release-apt.py
```

Test locally first:

```sh
./release-apt.py --dry-run    # build repo, skip upload
./release-apt.py --serve      # build and serve via HTTP (port 8080)
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
- `createrepo_c` for generating RPM metadata (`apt install createrepo-c` on
  Debian/Ubuntu)

### Publishing

Build the RPM repository, sign the metadata, and upload to R2:

```sh
./release-rpm.py
```

Test locally first:

```sh
./release-rpm.py --dry-run    # build repo, skip upload
./release-rpm.py --serve      # build and serve via HTTP (port 8080)
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

Pushes PKGBUILD and `.SRCINFO` to `promptr-git` and `promptr-bin` on the AUR.

```sh
./release-aur.py --type git     # for source package
./release-aur.py --type bin     # for binary package
./release-aur.py --type both    # both (default)
```

Requires an SSH key registered with your AUR account.

## Cloudflare Cache

The domain `packages.toxdes.com` serves R2 via Cloudflare with long cache TTLs.
After publishing, purge the cache so users get the latest packages:

```sh
# Check current TTL settings
./cf-purge-cache.py --check-ttl

# Set TTLs to 1 year (run once)
./cf-purge-cache.py --set-ttl

# Purge cache after publishing
./cf-purge-cache.py

# Dry-run (show what would happen)
./cf-purge-cache.py --dry-run
```

All commands load credentials from an env file:

```sh
./cf-purge-cache.py --env <path-to-env-file>
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
