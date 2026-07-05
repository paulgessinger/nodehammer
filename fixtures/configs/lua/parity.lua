-- Parity fixture (Lua side). Must produce byte-for-byte the same NHConfig as
-- parity.toml — see tests/config/test_lua_config.cpp. Exercises every field so
-- the cross-front-end equivalence test catches key/semantics drift.
config { hoist_orphans = true, deduplicate_shapes = false }

export("gltf", { unit_scale = 0.1, bake_unit_scale = true, multi_scene = true,
                 scene_name_separator = "/" })
export("obj", { unit_scale = 1.0 })

material("glass", {
  base_color         = "#88AACCFF",
  metallic           = 0.0,
  roughness          = 0.1,
  double_sided       = true,
  emissive           = { 0.1, 0.2, 0.3 },
  alpha_mode         = "blend",
  ior                = 1.5,
  transmission       = 0.9,
  clearcoat          = 0.5,
  clearcoat_roughness = 0.2,
  anisotropy         = 0.3,
  anisotropy_rotation = 0.7,
  specular           = 0.8,
  specular_color     = { 0.9, 0.8, 0.7 },
})

material("metal", {
  base_color   = "#60666E",
  metallic     = 1.0,
  roughness    = 0.05,
  alpha_mode   = "mask",
  alpha_cutoff = 0.3,
})

keep { 'path ~= "**/A"', 'path ~= "**/B"' }
drop { 'tag.sensitive == "false"' }

rule { match = 'name ~= "M1*"', material = "metal",
       tessellation = { max_segments_circle = 24, fallback = "bbox" } }
rule { match = { 'path ~= "**/X"', 'path ~= "**/Y"' },
       tessellation = { skip_geometry = true } }
rule { match = 'path ~= "/world"',
       tessellation = { merge_descendants = true, drop_coincident_faces = true, fallback = "fail" },
       extras = { visible = true, opacity = 0.5 } }

defaults {
  tessellation = { max_segments_circle = 10, fallback = "skip" },
  extras       = { visible = true, opacity = 1.0 },
}
