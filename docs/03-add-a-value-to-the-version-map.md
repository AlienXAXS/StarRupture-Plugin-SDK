# Add a value to the version map

Page 2 reads values by key. Sometimes the key you need has no row yet: a game function nobody asked for before, or a member the game does not describe itself. This page shows how to find the value, check it, and send it to us. We publish the row before your plugin is released, so your plugin can be a multi-version plugin from its first release.

You do not port anything by hand and you do not send a pull request. You send the values in an issue, and we write, sign and publish the rows. We sign and publish them from our own pipeline, which is why they do not come in as a pull request.

## What a row is

The version object map (version map for short) has one row per key per game build.

- A function row has the key, the build, a byte pattern and the function's address (an RVA, the offset from the start of the game image).
- A field row has the key, the build, an offset, a size and, for a bool bit, a mask.
- A struct size is a field row with the struct's name as the key, offset 0 and the size.

Keys are named like this:

| What | Key | Example |
|---|---|---|
| A game function or global | `Class::Function` or the global's name | `AActor::BeginPlay` |
| A spot inside a function | `<your PluginInfo name>/<hookName>` | `MyPlugin/MyHook` |
| A member | `Class::Member` | `UTexture::SRGB` |
| A struct size | `Class` | `FMyStruct` |

Use the Dumper-7 spelling for class and member names, the same as in the SDK headers.

Most members need no row at all. The game describes them itself, and the loader reads their offsets live. You only need a row for a function, a global, a spot inside a function, a member the game does not describe, or the size of a plain C++ struct. Page 2 says which is which.

## Find the value

1. Open the game exe in a disassembler and find the function or member.
2. For a function, copy the first bytes (about 20 to 60) into a pattern. Write them as upper-case hex bytes with single spaces, and put `??` for bytes that change, such as call targets and addresses. It must match exactly once in the exe.
3. For a member, find the offset and size, and the bit mask for a bool.
4. Write down the RVA of the function, or the offset and size of the member, for that build.

You can read members and struct sizes out of the game's PDB with `sr-aobscan`, if you have the PDB that matches your exe. It is built from the ModLoader repository (`tools/AobScan`; the project file lists the msbuild commands). Put one key per line in a text file, `Class::Member` or `Class` for a size, and run:

```
sr-aobscan-client.exe <game.exe> --layout keys.txt
```

It prints the offset, size and bool mask of every key, and which keys it could not find.

## Check it on every current build

A row that works on one build and not on the others breaks the plugin on the others. Two builds can be live at once. The CL numbers it has rows for are in `StarRupture-ModLoader-Core/hooks/game/version_map.generated.h`. Keep the exe of each build you test.

1. Members and sizes: run the `--layout` command above on each build's exe. Check that the numbers you want to send are right on every build, and that the bool mask is the same.
2. Functions: `sr-aobscan` can rescan a pattern on another build. Put your row for one build into a copy of the loader's own file, `StarRupture-ModLoader-Core/hooks/game/version_map.generated.h` in the [ModLoader repository](https://github.com/AlienXAXS/StarRupture-ModLoader). It is not a plugin header. Then run, for each other build:

   ```
   sr-aobscan-client.exe <exe of that build> --carry --map <the copy> --cl <that build's CL>
   ```

   For every key that has no row for that build, it prints a JSON report with the address your pattern finds there (`matches`, `rva`, `result`). Anything but one match means the pattern is not good enough: make it longer or wildcard more bytes, then check again.
3. Open the function in your disassembler on at least one other build and confirm the address `sr-aobscan` printed is the same function.
4. A function that moves between builds is normal, and each build gets its own row. A pattern that matches twice, or lands on another function, is not acceptable.

## Write the row

We use `ci/version-map.mjs` from [starrupture-ci](https://github.com/msantoro12/starrupture-ci) to write rows, so the header stays in one format. You can run it on a copy to see what we will publish, but you do not have to.

A function, on one build (hex values start with `0x`):

```
node ci/version-map.mjs add-function version_map.generated.h --key <key> --cl <CL> --rva <hex> --pattern "<bytes>" --kind 1
```

Run these on a copy of the loader's `version_map.generated.h`, the file named in "Check it". `--kind` only fits that file. `--kind 1` is a function start. A global is `--kind 4`. A key with a `/` in it is a spot inside a function; those carry another kind and an `--fp`, and we fill those in when we write the row. If the row has no pattern and only the RVA, use `--rva-only` instead of `--pattern`. The loader accepts such a row only on that exact exe.

If the pattern's address is a call you follow, add `--follow-rel32-at <n>`, the offset of the `E8` or `E9`.

A member, on one build:

```
node ci/version-map.mjs add-field version_map.generated.h --key UTexture::SRGB --cl 130045 --offset 0x106 --size 0x1 --mask 0x01
```

That is the row we publish for `UTexture::SRGB` on every current build. Leave `--mask` out for anything that is not a bool bit.

Then check the whole file:

```
node ci/version-map.mjs check version_map.generated.h
```

The same row added twice changes nothing. A row with other values for the same key and build is an error, so the tool never rewrites a row an earlier run checked.

## Send it to us

1. Open an issue on the [SDK repository](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/issues). Title it with your plugin's name and "version map rows".
2. List each key with its kind (function, global, spot, member or struct size).
3. For each key and build, give what you found: the pattern and RVA, or the offset, size and mask. Say how you checked it (the `sr-aobscan` output or the disassembler address). Say which of the four builds you checked it on.
4. Say if a key is optional: your plugin runs without it, and turns one feature off.

We check the values, add the rows to the loader's version map and publish them. The version map goes out on its own, so no loader release is needed, and the rows are live before your plugin is released. When we have published them, we comment on the issue. Then you release your plugin: it reads its values from the version map, sets `PLUGIN_MAP_MIGRATED` (page 2) and loads on every current build.

We will offer worked examples if you want to move an existing plugin over, but you do not have to do that port on your own before asking for rows.

## Worked example: AActor::BeginPlay

`AActor::BeginPlay` is a game function that a plugin may want to hook, and the loader's own version map has rows for it. You can open the file and compare every step below with the real rows: `StarRupture-ModLoader-Core/hooks/game/version_map.generated.h` in the [ModLoader repository](https://github.com/AlienXAXS/StarRupture-ModLoader), on `main`. Search it for `// AActor::BeginPlay`.

1. The key. It is a game function, so `AActor::BeginPlay`, kind 1.
2. The pattern. The first bytes of the function, with `??` where a byte changes:

   ```
   48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 0F B6 41
   ```

   It is the same on every build the version map covers.
3. The address. The function sits at a different RVA on each build:

   | CL | RVA |
   |---|---|
   | 127004 | `0x0452B260` |
   | 128786 | `0x04531830` |
   | 129402 | `0x045315D0` |
   | 130045 | `0x04530F50` |

4. The check. Run the `--carry` command from the section above on the exe of each build other than the one you found the function in. A good pattern gives exactly one match there, at the RVA in the table. Then open that address in your disassembler on one build and confirm it is `AActor::BeginPlay`.
5. The rows, one command per build:

   ```
   node ci/version-map.mjs add-function version_map.generated.h --key AActor::BeginPlay --cl 127004 --rva 0x0452B260 --pattern "<the pattern above>" --kind 1
   node ci/version-map.mjs add-function version_map.generated.h --key AActor::BeginPlay --cl 128786 --rva 0x04531830 --pattern "<the pattern above>" --kind 1
   node ci/version-map.mjs add-function version_map.generated.h --key AActor::BeginPlay --cl 129402 --rva 0x045315D0 --pattern "<the pattern above>" --kind 1
   node ci/version-map.mjs add-function version_map.generated.h --key AActor::BeginPlay --cl 130045 --rva 0x04530F50 --pattern "<the pattern above>" --kind 1
   ```

   In the header the first one reads:

   ```
   // AActor::BeginPlay
   { 127004, 0xE8972298, 0x0F35C000, 1, 0x00000000, false, "<pattern>", 0, -1, -1, 0x0452B260, nullptr },
   ```

   The image stamp and size (`0xE8972298`, `0x0F35C000`) are filled in from the build table, so the build must already be in it.
6. The plugin. It asks for the key by name, as on page 2, and carries no pattern and no address.

A member row is shorter. `UTexture::SRGB` has the same row on all four builds, offset `0x106`, size 1, mask `0x01`:

```
// UTexture::SRGB
{ 127004, 0x106, 0x1, 0x01, nullptr },
```

## Checklist

1. The key is named the way the table above says.
2. The pattern matches exactly once, on every current build, and finds the same function.
3. Members and sizes were checked with `--layout` on every build.
4. You sent the values in an issue, with the way you checked them.
5. You release your plugin after we say the rows are published.
