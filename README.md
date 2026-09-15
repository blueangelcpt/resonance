# Resonance

A native C++20 MP3 library manager and player: catalogue, artwork, BPM, lyrics,
privacy and gain cleaning, and safe organisation — with a Qt 6 desktop
application and a CLI sharing one engine.

![The Resonance workbench](docs/images/workbench.png)

**Your music is read-only.** Every operation treats the source collection as
untouchable. Version 1 always writes organised *copies* to a separate output
root; it never modifies the original files. The writer refuses any destination
that resolves inside a source root, including through a symlink or a junction,
and it copies the MPEG payload byte for byte so a tag edit provably cannot alter
the audio.

## What it does

| | |
|---|---|
| **Catalogue** | Every discovered tag field in SQLite, including frames it cannot interpret, whose exact payloads are retained and written back unchanged. |
| **Albums** | Provisional grouping with evidence, flagging conflicts rather than guessing. Editions are kept apart; compilations are not split by performer. |
| **Artwork** | Ranks candidates by correctness, then condition, then resolution — never resolution first. A clean 600×600 asset beats a damaged 3000×3000 scan. Outputs a 600×600 JPEG at quality 75. |
| **BPM** | Local analysis. 90% agreement with existing tags on a held-out sample, 0% octave errors. Half/double disagreements go to review, never silent correction. |
| **Lyrics** | LRCLIB matching on artist, title, version *and* duration. "Not found", "instrumental" and "failed" stay three different things. |
| **Privacy** | Removes recognised purchase and account data; preserves public identifiers like MusicBrainz IDs. Unknown private frames go to review, never removal. |
| **Gain** | Removes ReplayGain and Sound Check; preserves gapless information; refuses entirely when MP3Gain undo data is present. |
| **Organisation** | The confirmed template, with collision detection and Windows filesystem preflighting. |
| **Playback** | Double-click a track to play it, with automatic advance to the next one; play, pause, stop, seek and volume; a VFD spectrum analyser following the real playhead. |

## Building

Requires a C++20 compiler, CMake 3.22+, and:

```bash
sudo apt install -y build-essential ninja-build cmake pkg-config \
  qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev libgl1-mesa-dev \
  libtag1-dev libsqlite3-dev libjpeg-dev libpng-dev zlib1g-dev \
  libcurl4-openssl-dev
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Binaries land at `build/src/MusicLibrary.Cli/resonance` and
`build/src/MusicLibrary.Desktop/resonance-desktop`.

The CLI links no Qt module and runs without a display server. Build it alone
with `-DML_BUILD_DESKTOP=OFF`.

## Using it

```bash
# Read-only inventory. Writes nothing to your music.
resonance scan --source /path/to/music --data ~/.local/share/resonance

# Group albums, then see what needs a human decision.
resonance albums --source /path/to/music --data ~/.local/share/resonance
resonance review --source /path/to/music --data ~/.local/share/resonance

# Local analysis: no network, no writes to music.
resonance analyse --source /path/to/music --data ~/.local/share/resonance

# Enrichment. Degrades cleanly with --offline.
resonance artwork --source /path/to/music --data ~/.local/share/resonance
resonance lyrics  --source /path/to/music --data ~/.local/share/resonance

# Compute the exact changes. This command cannot modify a file.
resonance plan --source /path/to/music --output /path/to/output \
               --data ~/.local/share/resonance

# Write the copies, then verify them.
resonance export-copies --source /path/to/music --output /path/to/output \
                        --data ~/.local/share/resonance
resonance verify --source /path/to/music --output /path/to/output \
                 --data ~/.local/share/resonance
```

`resonance --help` lists every command. Exit code `5` means the run completed
but items need review before they can be written.

The desktop application takes the same paths:

```bash
resonance-desktop --source /path/to/music --output /path/to/output
```

## Using the desktop application

The window has four tabs. They are four views of the same catalogue, not four
separate tools — nothing you do in one requires leaving it to finish in
another.

### Library & commands

Where a library is opened and every operation is run, in order:

1. **Folders.** Set **Source** (your music — never written to), **Output**
   (where organised copies land) and **Catalogue and cache** (where the
   database, artwork assets and logs live). Output and cache must both sit
   outside the source. Tick **Offline** to skip every network lookup;
   cataloguing, local BPM analysis, local artwork processing and tagging
   copied files all still work.
2. Press **Open library**.
3. Run the commands top to bottom as numbered — each stage feeds the next:
   - **Read-only**: `1. Scan` inventories the source (writes nothing to your
     music); `2. Group albums` clusters files into provisional albums.
     `Create test sample` copies an independent sample elsewhere, for trying
     things without touching the real collection.
   - **Analyse**: `3. BPM (local)`, `4. Artwork`, `5. Lyrics` — enrichment.
     Artwork and lyrics degrade cleanly offline; BPM is always local.
   - **Plan and write**: `6. Build change plan` computes the exact changes
     (it cannot modify a file — there is no writer behind it). `7. Export
     copies` writes organised copies to the output folder. `8. Verify`
     re-reads what was written and checks it against the plan.
     `Resume after interruption` reconciles operations a crash or power loss
     left mid-flight.
4. **Progress** shows what is running right now, with a counter
   (`done of total`) once the total is known; while it is not yet known
   (for example, early in a scan before the file count is final) the bar
   shows indeterminate activity rather than sitting at zero, so it never
   looks stalled. The same progress also appears on the **Jobs & history**
   tab, which keeps a running log and adds **Pause** (finishes or discards
   the file in progress, then waits — nothing is ever left half-written) and
   **Cancel**.
5. **Coverage** summarises what fraction of the catalogue has artwork,
   lyrics, BPM and so on; **Refresh** recomputes it.

### Workbench

The main view once a library is open.

- **Library explorer** (left): a tree of the catalogue — *Needs attention*
  breaks out files missing artwork, lyrics or BPM, carrying gain fields,
  flagged for privacy, or unreadable; *Albums* lists every grouped album.
  Selecting a node filters the track matrix to it; the box above the tree
  filters the tree itself.
- **Selected track / VFD spectrum analyser / track matrix** (centre): the
  track matrix is the master list. **Click** a row to select it — this
  updates the deck, the inspector and the change-plan preview everywhere
  else in the window. **Double-click** a row to play it immediately,
  whatever the player was doing a moment before. The checkboxes above the
  matrix (*No artwork*, *No lyrics*, *No BPM*, *Gain*, *Privacy*) filter it
  further, alongside the free-text search box.
  The transport under **Selected track** is ▶/❚❚ (play/pause), ■ (stop), a
  volume slider, and **Analyse**, which decodes the selected track and shows
  its real spectrum in the VFD panel. While a track is actually playing, the
  analyser instead follows the live output; pausing freezes it on its last
  frame rather than blanking it, and it reverts to the decoded view on an
  actual stop. When one track finishes, the next row in the (filtered) track
  matrix starts automatically; at the end of the list, playback simply stops.
- **Inspector** (right): three tabs on the selected file — **Change plan**
  (what would be written and why, without writing anything), **ID3
  inspector** (the raw discovered tag frames, including ones this
  application does not interpret) and **Lyrics**.

### Artwork review

One album at a time: pick it from the table at the top (**Only albums
needing review** narrows that list). Below, the **Candidates** list holds
every artwork match found for that album; selecting one compares it against
what is currently embedded and previews the actual 600×600 output — the same
pipeline the writer uses, not an approximation. **Zoom** switches between
fit-to-pane (fair comparison across differently sized sources) and 100%
original pixels (where JPEG blocking, moiré and sharpening halos actually
show). **Lock this cover** pins a choice so it survives rescans and provider
refreshes; **Reject** discards a candidate for this album; **Import file…**
brings in a JPEG or PNG you already have, no network required. **Google
Images**, **Apple Music** and **MusicBrainz** open a search in your browser
as a manual research aid — no search API is called on your behalf.

### Jobs & history

The progress bar, counter, **Pause**/**Cancel** and a scrolling log of every
command run this session, each with its outcome. See *Library & commands*
above for what pause actually does mid-write.

### Keyboard shortcuts

| Key | Action |
|---|---|
| <kbd>Space</kbd> | Play/pause the selected track |

That is the complete list today — everything else is mouse-driven. If you
rely on a shortcut that is not here, it does not exist yet rather than being
undocumented.

## How safety is enforced

Not by convention — structurally.

- `PathGuard` is the only way to obtain write permission. It resolves paths and
  compares device and inode, so a symlink or junction pointing into your music
  is refused even when the path text looks innocent.
- `plan` returns data and touches only the catalogue. It has no writer, so it
  cannot modify a file even if asked to.
- The writer copies the MPEG payload from the source and hashes it, then
  verifies the reopened output against that hash before publishing.
- Output is written to an exclusively-created temporary file, verified, and only
  then renamed into place. An interrupted run leaves no partial file.
- A preservation check compares the complete frame inventory before and after.
  Any unplanned change fails the write.

Measured on a 3240-file collection: every source file byte-identical after a
full scan and an export run.

## Documentation

| | |
|---|---|
| [`docs/requirements.md`](docs/requirements.md) | The functional requirements baseline (FRD v1.3), unchanged. |
| [`docs/traceability.md`](docs/traceability.md) | Every requirement, its status, and the evidence for it. |
| [`docs/progress.md`](docs/progress.md) | What was done, with exact commands and results. |
| [`docs/open-questions.md`](docs/open-questions.md) | What is **not** done, and what is blocked on what. |
| [`docs/adr/`](docs/adr/) | Decisions that deviate from the FRD's suggested components, with reasons. |

If you want to know what this actually does versus what it merely intends to do,
read `docs/traceability.md` and `docs/open-questions.md` together. The first
records what has been verified; the second records what has not.

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE), and `native/DEPENDENCIES.md` for
third-party components and their obligations.
