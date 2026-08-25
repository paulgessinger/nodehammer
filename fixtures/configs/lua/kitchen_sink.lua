-- Kitchen-sink scripted config. Demonstrates the whole surface: top-level
-- flags, per-format export overrides, include() of side-effect fragments (which
-- in turn use() a shared constants module), literal keep/rule tails, and global
-- defaults. `config flatten -c fixtures/configs/lua/kitchen_sink.lua` emits
-- flattened TOML that round-trips through ConfigLoader.
--
-- The predicates name ODD geometry because that is the detector on hand, but
-- this is a tour of the DSL rather than a port of any config: the fragments it
-- pulls in collapse ODD's fifteen materials onto three, and it omits the
-- drop-all baseline entirely. For a faithful translation of odd.toml, see
-- odd.lua.
config { hoist_orphans = true }

export("gltf", { unit_scale = 0.1, bake_unit_scale = true })  -- blender
export("obj",  { unit_scale = 1.0 })

include("materials.lua")
include("tracker.lua")
include("calorimeters.lua")
include("muon.lua")

keep { 'name == "BeamPipe"' }
rule { match = 'path ~= "/world"',    tessellation = { skip_geometry = true } }
rule { match = 'name ~= "Solenoid*"', tessellation = { max_segments_circle = 48 } }

defaults {
  tessellation = { max_segments_circle = 10, fallback = "skip" },
  extras       = { visible = true, opacity = 1.0 },
}
