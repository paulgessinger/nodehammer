# Single-instance viewer

Plan for restricting `nodehammer viewer` to one running instance per
machine, and the macOS `.app` bundle that pairs naturally with it.

This document is forward-looking — none of it is implemented yet. It's
referenced from
[viewer-project-strategy.md §13](viewer-project-strategy.md) ("things
deliberately not solved → multiple OS-level viewer windows").

---

## 1. Why

The viewer's project model assumes a single live project per process
(see strategy doc §4 — `ProjectMode`, `SaveTarget`, `ManifestOrigin`,
editor windows are all process-singletons). Two simultaneous viewer
processes would race the native bag's storage directory, fight over
filesystem watcher events, and create persistence conflicts that aren't
worth solving in the data layer.

The cleaner answer is to enforce single-instance at the OS level, so
the data layer can keep treating "one project per process" as a hard
invariant.

Scope: only the **`viewer` subcommand** is restricted. `convert`,
`inspect`, `config validate`, `convert` and any
other non-window subcommand keep running freely — they're stateless
and parallel-safe.

---

## 2. Two layers, both useful

| Layer                       | Covers                                                    | Required for                                                |
| --------------------------- | --------------------------------------------------------- | ----------------------------------------------------------- |
| Lockfile + IPC forward      | any invocation of the viewer subcommand from a terminal   | "I ran `nodehammer viewer foo.toml` from a second tab"      |
| macOS `.app` bundle (`LSMultipleInstancesProhibited`) | GUI launches: double-click, `open -a`, drag-onto-dock, "Open with…" | Finder integration, file associations, dock drops           |

Both are needed because they cover different launch surfaces. The
lockfile guards the CLI path even after the bundle exists; the bundle
gives the GUI launch path the right behavior (and brings file
associations as a bonus).

---

## 3. Layer 1 — lockfile + IPC forward

### 3.1 Behavior

On `nodehammer viewer` startup:

1. Open / create the platform lock path (§3.2) and try to acquire an
   exclusive advisory lock.
2. **Lock acquired**: continue normal startup; hold the lock for the
   process lifetime; clean it up on exit.
3. **Lock held by another process**: open a Unix domain socket
   (named pipe on Windows) that lives alongside the lock, send the
   command-line file arguments to the existing instance, print
   `forwarded N file(s) to running viewer`, and exit 0. If the socket
   isn't there or isn't responding, fall back to printing
   `viewer is already running (pid N)` and exit non-zero.

The existing instance accepts forwarded files and feeds them through
the same drop pipeline as drag-and-drop (`addProjectPath` /
`addProjectBytes` in `App`). The window optionally raises itself
(`platform_->raiseWindow()` — needs a thin per-platform helper, NSApp
`activate(ignoringOtherApps:true)` on macOS, similar on Linux/Windows).

### 3.2 Paths

| Platform | Lock path                                                        | IPC                                  |
| -------- | ---------------------------------------------------------------- | ------------------------------------ |
| macOS    | `~/Library/Application Support/nodehammer/viewer.lock`           | Unix socket at `viewer.sock`         |
| Linux    | `${XDG_RUNTIME_DIR:-/tmp}/nodehammer/viewer.lock`                | Unix socket at `viewer.sock`         |
| Windows  | named mutex `Local\\Nodehammer.Viewer`                           | named pipe `\\\\.\\pipe\\nodehammer-viewer` |

The lock and the socket sit in the same directory so a stale socket
(left over from a crash) can be detected and replaced when the lock is
re-acquired by a fresh process.

### 3.3 Crash safety

`flock` is advisory and tied to the open file descriptor, so kernel
cleanup releases it on crash. The socket needs explicit cleanup:
`unlink(socket_path)` in `atexit` plus a startup sweep that removes a
stale socket file if the lock is acquirable.

### 3.4 Implementation sketch

- Add `viewer/single_instance.{hpp,cpp}` with two entry points:
  - `acquireOrForward(args) -> {Acquired | Forwarded | AlreadyRunning}`
  - `serveForwardedDrops(App &)` — installs an async listener on the
    socket; received messages route through `App::addProjectPath`.
- Wire from the viewer entry point:
  ```cpp
  auto outcome = single_instance::acquireOrForward(argv);
  if (outcome == Forwarded || outcome == AlreadyRunning) return 0;
  // ... normal App startup
  single_instance::serveForwardedDrops(*app);
  ```
- Per-platform: macOS / Linux share a POSIX implementation
  (`flock` + `AF_UNIX`); Windows gets its own (`CreateMutexW` +
  `CreateNamedPipeW`).

Estimated effort: half a day for the POSIX path; another half day for
Windows when relevant.

### 3.5 Edge cases worth handling

- **`nodehammer viewer` with no file arguments while already running**:
  forward the empty list (the existing instance just raises its window).
- **The existing instance is hung** (lock held but socket not
  responding): time out the connect after ~500 ms and fall through to
  the "already running" message; do not auto-kill.
- **Forwarded paths that don't exist on the receiving instance's
  filesystem** (rare unless using SSH-shared homes): the existing
  instance reports the file as missing through the normal drop UI.

---

## 4. Layer 2 — macOS `.app` bundle

### 4.1 Why bundle

- **`LSMultipleInstancesProhibited`** in `Info.plist` makes Launch
  Services route every GUI invocation (Finder double-click,
  `open -a Nodehammer foo.toml`, drag-onto-dock, "Open with…") to the
  running instance via the `kAEOpenDocuments` Apple Event. No lockfile
  needed for those paths.
- **File associations** via `CFBundleDocumentTypes`: right-click a
  `.toml` / `.nhb` / `.nhb.zst` / `.zip` and "Open With → Nodehammer"
  works system-wide.
- **Dock-drop** to open files just works once associations are
  declared.
- **Dark mode** by default (`NSRequiresAquaSystemAppearance=false`) and
  retina rendering hints.
- **Spotlight metadata** if we declare a `CFBundleIdentifier`.

The CLI binary is unaffected — `Contents/MacOS/nodehammer` is the same
executable, still callable from a terminal for non-viewer subcommands.

### 4.2 `Info.plist` essentials

```xml
<key>CFBundleIdentifier</key>          <string>com.paulgessinger.nodehammer</string>
<key>CFBundleName</key>                <string>Nodehammer</string>
<key>CFBundleExecutable</key>          <string>nodehammer</string>
<key>CFBundlePackageType</key>         <string>APPL</string>
<key>LSMinimumSystemVersion</key>      <string>13.0</string>
<key>LSMultipleInstancesProhibited</key> <true/>
<key>NSHighResolutionCapable</key>     <true/>
<key>NSRequiresAquaSystemAppearance</key> <false/>
<key>CFBundleDocumentTypes</key>
<array>
  <dict>
    <key>CFBundleTypeName</key>            <string>Nodehammer config</string>
    <key>CFBundleTypeRole</key>            <string>Editor</string>
    <key>LSItemContentTypes</key>
    <array><string>org.tomlfoundation.toml</string></array>
  </dict>
  <dict>
    <key>CFBundleTypeName</key>            <string>Nodehammer scene</string>
    <key>CFBundleTypeRole</key>            <string>Editor</string>
    <key>LSItemContentTypes</key>
    <array><string>com.paulgessinger.nodehammer.nhb</string></array>
  </dict>
  <dict>
    <key>CFBundleTypeName</key>            <string>Nodehammer archive</string>
    <key>CFBundleTypeRole</key>            <string>Editor</string>
    <key>LSItemContentTypes</key>
    <array><string>public.zip-archive</string></array>
  </dict>
</array>
<key>UTExportedTypeDeclarations</key>
<array>
  <dict>
    <key>UTTypeIdentifier</key>            <string>com.paulgessinger.nodehammer.nhb</string>
    <key>UTTypeConformsTo</key>            <array><string>public.data</string></array>
    <key>UTTypeDescription</key>           <string>Nodehammer scene</string>
    <key>UTTypeTagSpecification</key>
    <dict>
      <key>public.filename-extension</key> <array><string>nhb</string><string>nhb.zst</string></array>
    </dict>
  </dict>
</array>
```

### 4.3 Apple Events plumbing

When Launch Services routes a second invocation to the existing
instance, it delivers `kAEOpenDocuments` to the app's Apple Event
dispatcher. sokol_app does not handle this directly; we register an
`NSAppleEventManager` handler in the platform layer:

```objective-c
[[NSAppleEventManager sharedAppleEventManager]
    setEventHandler:self
        andSelector:@selector(handleOpenDocs:withReplyEvent:)
      forEventClass:kCoreEventClass
         andEventID:kAEOpenDocuments];
```

The handler decodes the file URL list and hands each one to
`App::addProjectPath`. This is the same path the lockfile-IPC forward
uses, just delivered through a different transport.

### 4.4 CMake bits

```cmake
if(APPLE)
  set_target_properties(nodehammer PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "Nodehammer"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.paulgessinger.nodehammer")
endif()
```

`Info.plist.in` carries the template above, with `${MACOSX_BUNDLE_*}`
substitutions handled by CMake.

### 4.5 Code signing

For local development:

```
codesign --sign - --force --deep build/Nodehammer.app
```

Ad-hoc signature is enough to keep Gatekeeper from quarantining local
builds. Distribution would need a Developer ID and notarization — out
of scope for this plan.

### 4.6 Bundle vs. CLI symlink

Keep both paths working:

- The bundle is `build/Nodehammer.app`, with the binary at
  `Contents/MacOS/nodehammer`.
- A symlink (or a thin shim) at `build/nodehammer` points into the
  bundle so `./build/nodehammer convert …` from the project root keeps
  working unchanged.

---

## 5. Linux and Windows

These are not blocking the macOS work; capturing intent so the design
doesn't paint itself into a corner.

### Linux

- `.desktop` file in `~/.local/share/applications/nodehammer.desktop`
  with `MimeType=application/toml;application/zip;application/x-nodehammer-scene;`
  declares file associations.
- Single-instance: the lockfile + Unix socket from §3 is the whole
  story. There's no Launch Services equivalent.
- Custom MIME types via `xdg-mime install`.

### Windows

- Single-instance via the named mutex from §3.
- File associations through the installer (registry under
  `HKCU\Software\Classes\.toml\OpenWithProgIds` etc.). Skip until we
  have an installer story.
- Named pipe IPC instead of Unix socket.

---

## 6. Build order

1. **Lockfile-only first cut.** macOS + Linux POSIX implementation;
   on second invocation, print "viewer is already running" and exit.
   Ships single-instance behavior with minimum code.
2. **IPC forward** added to the lockfile path. Second invocation now
   forwards files to the running instance and exits.
3. **Window-raise helper** so the running instance focuses itself when
   it receives a forward.
4. **macOS bundle** with `LSMultipleInstancesProhibited` + file
   associations + Apple Events handler. Lockfile stays — terminal
   invocations still need it. Bundle covers GUI invocations.
5. **Linux `.desktop` file** + custom MIME types.
6. **Windows mutex + named pipe** when Windows becomes a target.

Each step is independently shippable.

---

## 7. Effect on the project strategy doc

[viewer-project-strategy.md §13](viewer-project-strategy.md) currently
flags multi-window identity as "deliberately not solved." Once steps 1
and 4 above ship, that caveat downgrades to a one-liner referencing
this document — the viewer is single-window by enforcement, not by
convention.

The web side stays unchanged: there's no browser equivalent of
`LSMultipleInstancesProhibited`, so the multi-tab discussion in §13's
"things deliberately not solved" remains the authoritative answer for
that platform.

---

## 8. Open questions

- **Window-raise on macOS without stealing focus from a frontmost app.**
  `NSApp activateIgnoringOtherApps:YES` is the easy answer but is mildly
  rude. A polite alternative is `requestUserAttention:` (bouncing the
  dock icon) when the user is in another app, full activation only when
  Finder triggered the open. Decide on the policy before shipping the
  Apple Events handler.
- **Forwarded-drop UX feedback.** When a second invocation forwards
  files, the *terminal* prints a confirmation — but the *viewer* could
  also flash a small banner ("Received foo.toml from terminal") so the
  user sees that the drop landed. Optional; nice-to-have.
- **Bundle-only distribution vs. dual binary distribution.** If we ship
  a `.dmg` with the bundle, do we also expose the CLI binary outside
  the bundle for power users? Symlink-into-bundle is the simplest path;
  Homebrew tap users will expect a separate non-bundled binary too.
  Decide when distribution becomes a real concern.
