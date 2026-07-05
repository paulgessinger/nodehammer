-- Top-level scripted config. Demonstrates the whole surface: top-level flags,
-- per-format export overrides, include() of side-effect fragments (which in
-- turn use() a shared constants module), literal keep/rule tails, and global
-- defaults. `config-lua -c fixtures/configs/lua/odd.lua` emits flattened TOML
-- that round-trips through ConfigLoader.
config { hoist_orphans = true }

export("gltf", { unit_scale = 0.1, bake_unit_scale = true })  -- blender
export("obj",  { unit_scale = 1.0 })

include("materials.lua")
include("tracker.lua")

keep { 'name == "BeamPipe"' }
rule { match = 'path ~= "/world"',    tessellation = { skip_geometry = true } }
rule { match = 'name ~= "Solenoid*"', tessellation = { max_segments_circle = 48 } }

defaults {
  tessellation = { max_segments_circle = 10, fallback = "skip" },
  extras       = { visible = true, opacity = 1.0 },
}
