-- ODD full detector configuration -- the Lua port of ../odd.toml.
--
-- This is a faithful translation, not a demonstration: it builds the same
-- scene as `fixtures/configs/odd.toml`, so the two can be compared directly.
-- `config flatten -c fixtures/configs/lua/odd.lua` and
-- `config flatten -c fixtures/configs/odd.toml` emit equivalent TOML.
--
-- For a tour of the scripting surface itself, see kitchen_sink.lua.
--
-- Include order matters: base.lua provides the drop-all rule, then subsystem
-- fragments add keep rules and specific settings. Global fallback values live
-- in defaults{} so they never override specific rules regardless of ordering.

config { hoist_orphans = true }

export("gltf", { unit_scale = 0.1, bake_unit_scale = true })  -- blender
-- export("gltf", { unit_scale = 10.0, multi_scene = true })  -- phoenix
export("obj", { unit_scale = 1.0 })

include("odd/base.lua")
include("odd/materials.lua")
include("odd/tracker.lua")
include("odd/calorimeters.lua")
include("odd/muon.lua")

-- ── Selection ────────────────────────────────────────────────────────────────

keep { 'name == "BeamPipe"' }

-- ── Rules ────────────────────────────────────────────────────────────────────

-- World node: no geometry.
rule { match = 'path ~= "/world"', tessellation = { skip_geometry = true } }

-- Solenoid: needs more phi segments.
rule { match = 'name ~= "Solenoid*"', tessellation = { max_segments_circle = 48 } }

-- Enable interior-face removal on every calorimeter stave.
rule {
  match = {
    'path ~= "**/ECalBarrel/stave*"',
    'path ~= "**/ECalEndcap_A/stave*"',
    'path ~= "**/ECalEndcap_B/stave*"',
    'path ~= "**/HCalBarrel/stave*"',
    'path ~= "**/HCalEndcap_A/stave*"',
    'path ~= "**/HCalEndcap_B/stave*"',
  },
  tessellation = { drop_coincident_faces = true },
}

-- Enable interior-face removal on long-strip barrel staves.
rule {
  match = 'path ~= "**/LongStripLayer*/stave*"',
  tessellation = { drop_coincident_faces = true },
}

-- ── Defaults ─────────────────────────────────────────────────────────────────
-- Global fallback for tessellation and extras — applies when no rule sets a
-- field, so specific rules in subsystem fragments always win.

defaults {
  tessellation = { max_segments_circle = 10, fallback = "skip" },
  extras       = { visible = true, opacity = 1.0 },
}
