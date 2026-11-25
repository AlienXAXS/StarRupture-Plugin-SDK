# Get listed in Browse

Browse is the tab in the loader window that lists mods. A player can search it, open a mod's page and, for a listed mod that has a download, press **Install**. The loader downloads the file from your GitHub release, checks its size and SHA-256 against the list, writes it to `ModLoader\Plugins` and loads it. If it can't load it right away, the card says "Installed. It will be on after you restart the game."

The list is one signed file. The loader reads an entry from it only after it checks the signature. This page shows what an entry is, how to fill it in and how to send it to us.

## What an entry is

One entry is one JSON object. These are all the fields the loader reads.

| Field | Needed | What it is |
|---|---|---|
| `id` | yes | Short, lowercase, up to 64 characters. Other mods use it in `requires`. |
| `name` | yes | The name on the card, up to 64 characters. |
| `page_url` | yes | Where **Open page** goes. `https://` on `github.com` or `nexusmods.com`, up to 200 characters, no port and no user name. |
| `author` | no | Up to 64 characters. Players can search it. |
| `version` | no | Your latest release, up to 32 characters. |
| `summary` | no | One or two sentences, up to 300 characters. Players search it, so use the words they would type. |
| `readme_url` | no | A raw link to your README on `raw.githubusercontent.com`. The card gets a **README** button. |
| `tested_loader` | no | The ModLoader version you tested on, for example `1.27.0`. |
| `interface_version_min` | no | The plugin interface number your plugin declares. Leave it out if you don't know it. |
| `requires` | no | The `id` of each mod yours needs, up to 8, for example `["minimap"]`. Install is refused until they are installed and enabled. The card does not list them, so say it in `summary` too. |
| `server_only` | no | `true` for a plugin that runs only on a dedicated server. It never installs on a player's PC. |
| `download_url`, `download_sha256`, `download_size`, `install_files` | for Install | The download. Give all four or none. |
| `update_manifest_url` | with the `.json` | See below. |

Without the download fields the card still shows, with **Open page** and no **Install**.

### The download

1. `download_url` is the `.dll` you attached to a GitHub release: `https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.dll`. It must be in the same repository as `page_url`. No other site works, and not a branch link or a zip. The loader follows GitHub's own redirect to `release-assets.githubusercontent.com` or `objects.githubusercontent.com` and to nothing else.
2. `download_sha256` is the SHA-256 of that file: 64 lowercase hex characters.
3. `download_size` is its size in bytes, a whole number from 1 to 33554432 (32 MB), written as plain digits (`2e6` is refused).
4. `install_files` is the name the DLL gets in `ModLoader\Plugins`, as a list. It has exactly one `.dll`, made of letters, digits, `_`, `.` and `-`, up to 80 characters. It can differ from the asset name. A name ending `-Server.dll` is not installed. Install never replaces a file that is already there.

To keep a plugin current by itself, also list its `.json` beside the DLL, for example `["MyMod.dll", "MyMod.json"]`, and add `update_manifest_url`: `https://github.com/<owner>/<repo>/releases/latest/download/<name>-client-manifest.json`, in the same repository. The `.json` and `update_manifest_url` go together, both or neither.

Never replace a file after it is listed. A new version is a new release with its own file. If the file behind a listed address changes, Install refuses it.

### Server-only mods

A plugin that runs only on a dedicated server is no use on a player's PC. Set `"server_only": true` and leave out the four download fields. The card shows **(Server)** after the author and an **Open page** button, and never **Install**. The check below refuses a `server_only` entry that has a download.

## Get the SHA-256 and the size

Download the `.dll` from your release page, so you hash the copy players get. Then, in a Windows command prompt, in the folder that holds it:

```
certutil -hashfile ConfirmHotkey-Client.dll SHA256
dir ConfirmHotkey-Client.dll
```

`certutil` prints the hash in lowercase. In PowerShell:

```
(Get-FileHash .\ConfirmHotkey-Client.dll -Algorithm SHA256).Hash.ToLower()
(Get-Item .\ConfirmHotkey-Client.dll).Length
```

## Worked example

This is the entry for ConfirmHotkey, the plugin from page 2. It is taken from `mod-index/index.json` in the ModLoader repository.

```json
{
  "id": "confirmhotkey",
  "name": "ConfirmHotkey",
  "author": "NasserHabib",
  "version": "2.4.0",
  "summary": "Adds a key (default E) that presses the confirm button in the Recycler and Analyzer.",
  "page_url": "https://github.com/NasserHabib/StarRupture-Plugin-ConfirmHotkey",
  "interface_version_min": 70,
  "readme_url": "https://raw.githubusercontent.com/NasserHabib/StarRupture-Plugin-ConfirmHotkey/main/README.md",
  "download_url": "https://github.com/NasserHabib/StarRupture-Plugin-ConfirmHotkey/releases/download/v2.4.0/ConfirmHotkey-Client.dll",
  "download_sha256": "b4a243649eaff22d429fff6679698ff8130b64d85bdf08f20a4ad51ebde22163",
  "download_size": 37376,
  "install_files": ["ConfirmHotkey.dll", "ConfirmHotkey.json"],
  "update_manifest_url": "https://github.com/NasserHabib/StarRupture-Plugin-ConfirmHotkey/releases/latest/download/ConfirmHotkey-client-manifest.json"
}
```

- The release `v2.4.0` of that repository carries `ConfirmHotkey-Client.dll`, and `download_sha256` and `download_size` are that file's hash and size.
- The asset is `ConfirmHotkey-Client.dll` and it is installed as `ConfirmHotkey.dll`.
- The `.json` is listed, so `update_manifest_url` is there, in the same repository.
- The `summary` holds the words a player would search: "key", "confirm", "Recycler", "Analyzer".

## Send it to us

1. Make the GitHub release first, with the `.dll` attached as its own file.
2. Work out the SHA-256 and the size, as above.
3. Write your entry.
4. Open an issue on this repository with the entry and a link to the release.
5. We check it, add it to the list and sign it. The list ships on its own, together with the version object map. No loader release is needed.

We refuse an entry that has any of these: a `download_url` that is not a release asset of the `page_url` repository, a `download_sha256` that is not 64 lowercase hex characters, a size out of range, an install field written twice or nested inside another value, or a `server_only` entry with a download. The list is at most 200 entries and 256 KB.

To see **Install** work before your entry is in the list, point your own copy of the loader at your file. In `ModLoader\modloader.ini`:

```
[AutoUpdate]
ModIndexUrl=https://example.com/my-index.json
```

The card then shows "This list isn't signed. It comes from the address you set yourself."

## What a player sees

- Search matches `name`, `author` and `summary`, in any case and any order. A word of 3 letters or more also matches a name with those letters in order, so "minmap" finds MiniMap.
- The list is not published yet: "The mod list isn't published yet."
- The list server fails: "The mod list server answered with an error (HTTP 503)." (the number the server sent). **Retry** tries again.
- The list is over the size limit: "The mod list is larger than allowed."
- A mod that can't install shows a greyed **Install** and the reason under the card:
  - "Vortex manages this mod. Install it in Vortex."
  - "MyMod needs MiniMap. Install MiniMap first." (or "Enable MiniMap first.")
  - "Needs a newer ModLoader." when `interface_version_min` is higher than the loader knows.
  - "This mod can't be installed from here. Open its page." for a `server_only` entry.
- The downloaded file does not match the list: "The downloaded file does not match the mod list, so nothing was installed."

Pictures are not needed yet. Browse shows a placeholder card for every mod.

## Checklist

1. The `.dll` is a file attached to a GitHub release, and you never replace it.
2. `download_url` and `page_url` name the same repository.
3. `download_sha256` is 64 lowercase hex characters and `download_size` is the size in bytes.
4. `install_files` has exactly one `.dll`.
5. A server-only mod has no download fields.
6. You sent the entry and the release link in an issue.
