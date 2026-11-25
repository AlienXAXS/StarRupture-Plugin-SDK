# Get listed in Browse

Browse is the tab in the loader window that lists mods. A player can search it, open a mod's page and, for a listed mod that has a download, press **Install**. The loader downloads the file from your GitHub release, checks its size and SHA-256 against the list, writes it to `ModLoader\Plugins` and loads it. If it can't load it right away, the card says "Installed. It will be on after you restart the game."

You get listed by adding one entry to `mod-index/index.json` in the loader repository, `AlienXAXS/StarRupture-ModLoader`, in a pull request. This page shows what an entry is and how to send it.

## What an entry is

One entry is one JSON object. These are all the fields the loader reads.

| Field | Needed | What it is |
|---|---|---|
| `id` | yes | Short, lowercase, up to 64 characters. Other mods use it in `requires`. |
| `name` | yes | The name on the card, up to 64 characters. |
| `page_url` | yes | Where **Open page** goes. `https://` on `github.com` or `nexusmods.com`, up to 200 characters, no port and no user name. To use Install, it must be your GitHub repository, `https://github.com/<owner>/<repo>`. |
| `author` | no | Up to 64 characters. Players can search it. |
| `version` | no | Your latest release, up to 32 characters. |
| `summary` | no | One or two sentences, up to 300 characters. Players search it, so use the words they would type. |
| `readme_url` | no | A raw link to your README on `raw.githubusercontent.com`. The card gets a **README** button. |
| `tested_loader` | no | The ModLoader version you tested on, for example `1.27.0`. |
| `interface_version_min` | no | The plugin interface number your plugin declares. Leave it out if you don't know it. |
| `requires` | no | The `id` of each mod yours needs, for example `["minimap"]`. Install is refused until they are installed and enabled. |
| `server_only` | no | `true` for a plugin that runs only on a dedicated server. It never installs on a player's PC. Leave out the download fields. |
| `download_url`, `download_sha256`, `download_size`, `install_files` | for Install | The download. Give all four or none. |
| `update_manifest_url` | with the `.json` | See below. |

Without the download fields the card still shows, with **Open page** and no **Install**.

### The download

- `download_url` is the `.dll` you attached to a GitHub release: `https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.dll`. It must be in the repository `page_url` names. Not another site, not a branch link, not a zip. A name ending `-Server.dll` is not installed.
- `download_sha256` is the SHA-256 of that file: 64 lowercase hex characters.
- `download_size` is its size in bytes, a whole number from 1 to 33554432 (32 MB), in plain digits.
- `install_files` is the name the DLL gets in `ModLoader\Plugins`, as a list. It has exactly one `.dll` (letters, digits, `_`, `.` and `-`, up to 80 characters). It can differ from the asset name.
- To keep the plugin current by itself, also list its `.json` (for example `["MyMod.dll", "MyMod.json"]`) and add `update_manifest_url`: `https://github.com/<owner>/<repo>/releases/latest/download/<name>-client-manifest.json`, in the same repository. The `.json` and `update_manifest_url` go together, both or neither.

Never replace a file after it is listed. A new version is a new release with its own file, and `version`, `download_url`, `download_sha256` and `download_size` change together. If the file behind a listed address changes, Install refuses it.

## Get the SHA-256 and the size

Download the `.dll` from your release page, so you hash the copy players get. In PowerShell, in the folder that holds it:

```
(Get-FileHash .\ConfirmHotkey-Client.dll -Algorithm SHA256).Hash.ToLower()
(Get-Item .\ConfirmHotkey-Client.dll).Length
```

## Worked example

This is the entry for ConfirmHotkey, the plugin from page 2, as it is in `mod-index/index.json`.

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

The asset is `ConfirmHotkey-Client.dll` and it is installed as `ConfirmHotkey.dll`. The `.json` is listed, so `update_manifest_url` is there, in the same repository.

## Send it to us

1. Make the GitHub release first, with the `.dll` attached as its own file. The release workflow on page 5 attaches it as `<PluginName>-Client.dll`.
2. Work out the SHA-256 and the size, as above.
3. Fork `AlienXAXS/StarRupture-ModLoader` and add your entry to the array in `mod-index/index.json`. Keep LF line endings and a newline at the end.
4. Open a pull request.
5. An automatic check runs on the pull request. It checks that:
   - the entry's fields are the right shape, with the limits above;
   - the release asset exists at `download_url`, and its size and SHA-256 match the entry;
   - `page_url` and `download_url` point at the same GitHub repository;
   - the file is a 64-bit Windows DLL with the plugin entry points the loader needs.

   It only reads the file. It never runs your plugin. If it fails, the message names the entry and the rule, so fix the entry and push again.
6. After the pull request is merged, your entry shows up in Browse with the next signed mod list update. No loader release is needed.

## What a player sees

- Search matches `name`, `author` and `summary`, in any case and any order. A word of 3 letters or more also matches a name with those letters in order, so "minmap" finds MiniMap.
- A mod that has a download but can't install shows a greyed **Install** and the reason under the card, for example "MyMod needs MiniMap. Install MiniMap first." or "Needs a newer ModLoader." when `interface_version_min` is higher than the loader knows.
- If the downloaded file does not match the list, nothing is installed.

## Checklist

1. The `.dll` is a file attached to a GitHub release, and you never replace it.
2. `download_url` and `page_url` name the same repository.
3. `download_sha256` is 64 lowercase hex characters and `download_size` is the size in bytes.
4. `install_files` has exactly one `.dll`.
5. A server-only mod has no download fields.
6. You opened a pull request that adds your entry to `mod-index/index.json`.
