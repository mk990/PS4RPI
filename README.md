# ps4_remote_pkg_installer-OOSDK

This is a OOSDK port of flat's ps4_remote_pkg_installer, all credit goes to them and their amazing work!

RPI runs an HTTP server on the console. You POST it the URL of a PKG hosted
somewhere on your network, and it hands the transfer to the system's own
background download service (BGFT), which installs the package as if it had
come from the store.

- Default port: **12801**
- Working directory: `/data`
- The IP and port are shown in a notification when the app starts.

## Building

A visual studio project has been included for building on Windows. On Linux, a makefile has been included.

To build this project, the developer will need clang, which is provided in the [toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain). The `OO_PS4_TOOLCHAIN` environment variable will also need to be set to the root directory of the SDK installation.

__Windows__
Open the Visual Studio project and build, or run the batch file from command prompt or powershell with the following command:

```bash
.\build.bat .\x64\Debug "RPI" "%OO_PS4_TOOLCHAIN%\\RPI"
```

__Linux__
Run the makefile.

```bash
make
```

__MacOS__
Run the makefile.

```bash
brew install llvm
sudo OO_PS4_TOOLCHAIN=/opt/OpenOrbis-PS4-Toolchain make
```

If `PkgTool.Core` aborts with "Couldn't find a valid ICU package installed on
the system", it is the toolchain's .NET binary refusing to start on a machine
without ICU. Either install ICU, or build with:

```bash
DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 make
```

If the build instead aborts at `pkg_build` with "No usable version of libssl
was found", the same binary is a self-contained .NET Core 3.0 build whose crypto
layer can only load OpenSSL 1.0/1.1 — it does not understand the OpenSSL 3 that
current distributions ship. Install your distribution's OpenSSL 1.1 compatibility
package (`openssl-1.1` on Arch, `libssl1.1` on Debian/Ubuntu) or unpack one next
to the build and point the loader at it:

```bash
LD_LIBRARY_PATH=/path/to/openssl-1.1/lib make
```

### Other make targets

```bash
make compile_commands   # compile_commands.json for clangd, using the real build flags
make clean
```

### Checking sources without the SDK

`ci/syntax-check.sh` parses every first-party source file with a host clang
against the stub headers in `ci/stubs`, using the same warning set the real
build uses. It needs nothing but clang and catches format-string mismatches,
unused labels, uninitialised locals and signature drift:

```bash
./ci/syntax-check.sh
```

It is a lint, not a build — it knows nothing about the real SDK's struct
layouts, and `installer.c`/`module.c` are skipped for that reason. GitHub
Actions runs it on every push.

## API

All endpoints accept both `GET` and `POST`, except `/static/` which is `GET`
only. `OPTIONS` is answered for CORS preflight.

- **POST**: the JSON object goes in the request body.
- **GET**: the same JSON object goes in a `data` query parameter, URL-encoded.
  The value is limited to 2048 bytes.

Every response is JSON. Success:

```json
{ "status": "success" }
```

Failure, when the call itself was rejected:

```json
{ "status": "fail", "error": "human readable reason" }
```

Failure, when a system call returned an error code:

```json
{ "status": "fail", "error_code": 0x80990088 }
```

### `/api/install`

Registers a download task. Two flavours, selected by `type`.

`type: "direct"` — you supply the piece URLs yourself. A package split into
multiple files is listed in order.

```json
{
  "type": "direct",
  "packages": ["http://192.168.1.10/game.pkg"]
}
```

`type: "ref_pkg_url"` — you supply the URL of a reference-package JSON, and RPI
fetches it and extracts the piece URLs from it.

```json
{
  "type": "ref_pkg_url",
  "url": "http://192.168.1.10/game.json"
}
```

Both accept an optional `ssl_verify` boolean that overrides the configured TLS
policy for this one install (see [TLS verification](#tls-verification)).

Response:

```json
{ "status": "success", "task_id": 3, "title": "Some Game" }
```

The returned `task_id` is what the task endpoints below operate on. Registering
a task does not start it — call `/api/start_task`.

URLs may contain spaces and non-ASCII characters; RPI percent-encodes them
before handing them to the download service.

### `/api/start_task`, `/api/stop_task`, `/api/pause_task`, `/api/resume_task`, `/api/unregister_task`

```json
{ "task_id": 3 }
```

Response: `{ "status": "success" }`.

### `/api/get_task_progress`

```json
{ "task_id": 3 }
```

Response:

```json
{
  "status": "success",
  "bits": 0x1,
  "error": 0,
  "length": 0x1F400000,
  "transferred": 0x3E80000,
  "length_total": 0x1F400000,
  "transferred_total": 0x3E80000,
  "num_index": 0,
  "num_total": 1,
  "rest_sec": 240,
  "rest_sec_total": 240,
  "preparing_percent": 100,
  "local_copy_percent": 0
}
```

Sizes are hex; percentages and counts are decimal.

### `/api/find_task`

Looks up an existing task for a content id.

```json
{ "content_id": "UP0000-CUSA00000_00-0000000000000000", "sub_type": 0 }
```

Response: `{ "status": "success", "task_id": 3 }`.

### `/api/is_exists`

```json
{ "title_id": "CUSA00000" }
```

Response, when installed (`size` in hex, `0xFFFFFFFFFFFFFFFF` if it could not be
determined):

```json
{ "status": "success", "exists": "true", "size": 0x1F400000 }
```

### `/api/uninstall_game`, `/api/uninstall_patch`

```json
{ "title_id": "CUSA00000" }
```

### `/api/uninstall_ac`, `/api/uninstall_theme`

```json
{ "content_id": "UP0000-CUSA00000_00-0000000000000000" }
```

### `/api/settings`

`GET` with no payload returns the current settings:

```json
{
  "status": "success",
  "ssl_verify": true,
  "ca_bundle": "/data/rpi_cacert.pem",
  "ca_bundle_loaded": true
}
```

`POST` (or `GET` with a `data` payload) updates them. Both fields are optional;
what you send is persisted to `/data/rpi_config.json` and survives a restart.

```json
{ "ssl_verify": false, "ca_bundle": "/data/my_roots.pem" }
```

Setting `ca_bundle` loads that file immediately and fails the request if it
cannot be read or parsed.

### `/static/<file>`

Serves files out of the working directory. This is how the download service
fetches the reference JSON and icon that `/api/install` writes; it is not a
general-purpose file server. `.json` and `.png` get their proper content types,
everything else is `application/octet-stream`.

## TLS verification

Package servers reached over `https://` have their certificate verified by
default: the chain, the host name, and the validity period are all checked.

The console's built-in root store is old, so certificates from current issuers
will often fail against it. To fix that, put a concatenated PEM bundle of the
roots you trust at `/data/rpi_cacert.pem` — it is picked up at startup, and
`/api/settings` can point at a different path. When an install fails on a TLS
problem, the error string says so and names the bundle path.

If you are serving packages from a host with a self-signed certificate, you can
turn verification off — globally via `/api/settings`, or for a single install by
passing `"ssl_verify": false` to `/api/install`. Plain `http://` servers are
unaffected either way.

## Examples

```bash
# Install a package and start downloading it
curl -s -X POST http://192.168.1.20:12801/api/install \
  -d '{"type":"direct","packages":["http://192.168.1.10/game.pkg"]}'
# => { "status": "success", "task_id": 3, "title": "Some Game" }

curl -s -X POST http://192.168.1.20:12801/api/start_task -d '{"task_id":3}'

# Watch it
curl -s -X POST http://192.168.1.20:12801/api/get_task_progress -d '{"task_id":3}'

# Install from a host with a self-signed certificate
curl -s -X POST http://192.168.1.20:12801/api/install \
  -d '{"type":"direct","packages":["https://nas.local/game.pkg"],"ssl_verify":false}'

# The same request as a GET
curl -s -G http://192.168.1.20:12801/api/is_exists \
  --data-urlencode 'data={"title_id":"CUSA00000"}'
```

## NOTES

- The default port is 12801
- Some freeBSD net functions where giving issue with printing debug info out, so i swapped these to the sceNet* versions as they seemed to work.
- The server has no authentication and sends `Access-Control-Allow-Origin: *`.
  Anything that can reach the port can install and uninstall content, so keep it
  on a network you trust.

## Thanks

- [OpenOrbis/OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- [flatz/ps4_remote_pkg_installer](https://github.com/flatz/ps4_remote_pkg_installer)
