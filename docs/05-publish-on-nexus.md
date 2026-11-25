# Publish on Nexus

Most players find a mod on Nexus Mods. This page is for when your plugin has a released version on GitHub and you want it on a Nexus page. The order never changes: the GitHub release comes first, Nexus second.

The Nexus page is yours. You make it, you write its text and you keep it up to date. We do not run it for you.

## How the two fit together

- The GitHub release is the source of the files. Its ZIP is the file players download from Nexus too, so both carry the same version number.
- A Nexus page or file is updated only for a version that is already a GitHub release. Work that is not released yet stays off Nexus.
- The release workflow does both in one run. It creates the GitHub release, and then uploads the same ZIP to Nexus. So the order is kept for you, as long as you use it.

## What the release workflow makes

Copy `templates/release.yml` from this repository to `.github/workflows/release.yml` in your plugin repo, fill in the `TODO` values, and pin the SDK release tag. Run it from the Actions tab. Tick **dry_run** first: it builds and packages everything and publishes nothing.

A run makes:

- a version tag, generated from the last one,
- `<PluginName>-Client-vX.Y.Z.zip` with the DLL and its sidecar JSON, and `<PluginName>-client-manifest.json`, the update manifest,
- the same for the Server variant, if you set `server_build: true`,
- a GitHub release with all of them.

The release notes come from `UPDATE_NOTES.txt` in the repo root, one change per line. The same lines are shown on the release page, stored in the manifest, and used as the Nexus changelog entry. Without the file, the entry links to the release.

## Set up the Nexus upload

1. Make your Nexus page on nexusmods.com.
2. Put the page's global (v3 API) mod id in `nexus_mod_id` in your `release.yml`. Without it the workflow skips Nexus.
3. Add these secrets to your repo: `NEXUS_APIKEY` (your Nexus API key), `NEXUS_CLIENT_GROUP_ID`, and with `server_build` also `NEXUS_SERVER_GROUP_ID`. They are the ids of the file groups on your page.
4. Run the workflow. For each upload, it sets the page's Version field, the changelog and the description.

## What the Nexus page needs

The description is BBCode. The workflow's `nexus_description_file` input names the file it uploads as the page description (default `nexus/description.bbcode`); when that file exists, it replaces the page description on every upload. A page has these parts, in this order:

1. A title line, and one or two sentences on what the mod does.
2. Whether it is client side, server side or both.
3. Where to report a bug, and which log to attach. The loader's log is `ModLoader.log`.
4. Features.
5. Installation: the loader version it needs, then the steps to copy the files and set up the config.
6. Configuration and troubleshooting, only what a player needs.
7. Credits.

Credits go in one section at the bottom of the page and of your README. Name the original author first, then the maintainer if there is one, then the contributors as a short list of GitHub handles. No emails.

## Text for the next release

The page must describe the version that is out, not the one you are working on. A habit that works: keep two texts.

- `description.bbcode` is the text of the released version. This is the file the workflow uploads.
- `description-next.bbcode` is the text for the version you are building. Write new features in it as you add them. The workflow never reads it, and where you keep it is up to you.

When the GitHub release ships, copy `description-next.bbcode` over `description.bbcode`. Do not edit `description.bbcode` before then.

## The main image

The main image (the thumbnail at the top of the Nexus page) is not part of the release and the workflow cannot set it. You upload it by hand on your Nexus page, and so the gallery. The Nexus API cannot read or change either. Use the same screenshots on GitHub and on Nexus, so the Browse card, the release and the page match.

## What you do and what you ask us for

You do:

- the GitHub release, with `UPDATE_NOTES.txt`,
- your Nexus page and its text,
- the upload to your own Nexus page, with your own secrets.

Ask us on an issue in this repository for:

- version map rows your plugin needs (page 3),
- a Browse entry (page 4).

## Worked example: the next-release text for KeepTicking

KeepTicking is a server plugin. This is the shape of its `description-next.bbcode`, cut down to the parts every page has:

```
[size=6][b]KeepTicking — Keep Your Dedicated Server Running for StarRupture[/b][/size]

[b]Found a bug? Report it on GitHub: [url=https://github.com/AlienXAXS/StarRupture-Plugin-KeepTicking/issues]https://github.com/AlienXAXS/StarRupture-Plugin-KeepTicking/issues[/url][/b] and attach your [b]ModLoader.log[/b] (in [b]Binaries\Win64\ModLoader\Logs\[/b]).

Stops a StarRupture dedicated server from sleeping when no players are online. Without it, every machine on the server stops producing the moment the last player disconnects.

[b]Dedicated server only[/b]

[line]

[size=5][b]Installation[/b][/size]

[list=1]
[*]Install the mod loader (1.25.0 or newer) first.
[*]Download the server file from the [b]Files[/b] tab and open the ZIP.
[*]Copy the contents of the included Plugins folder into:
[i]"StarRupture\Binaries\Win64\ModLoader\Plugins"[/i]
[*]Launch the server once to generate the config file.
[*]Set [b]Enabled=1[/b] in:
[i]"Plugins\config\KeepTicking.ini"[/i]
[/list][line]

[size=5][b]Credits[/b][/size]

Original mod by [url=https://github.com/AlienXAXS]@AlienXAXS[/url]

Maintained by [url=https://github.com/msantoro12]@msantoro12[/url], creator of [url=https://starrupture.goodstuff.software]StarRupture Planner[/url]
```

The order is the one in "What the Nexus page needs": title and tagline, side, bug report, install, credits. When the release ships, this file replaces `description.bbcode`.

## Checklist

1. The version is already a GitHub release.
2. `UPDATE_NOTES.txt` lists the changes, one per line.
3. The description text matches the released version.
4. `nexus_mod_id` and the three secrets are set, or you upload by hand.
5. The main image and the gallery are on your page.
