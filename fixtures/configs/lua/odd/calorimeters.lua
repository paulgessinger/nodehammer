-- ODD calorimeters: ECal + HCal
--
-- The two calorimeters have the same shape -- a barrel and two endcaps, each
-- holding staves -- so the three keep blocks are emitted per calorimeter.

local calos = { "ECal", "HCal" }

-- The barrel/endcap suffixes, in the order the TOML fragment lists them.
local parts = { "Barrel", "Endcap_A", "Endcap_B" }

-- ── Selection ────────────────────────────────────────────────────────────────

for _, c in ipairs(calos) do
  -- The calorimeter itself and its barrel/endcap containers.
  local containers = { ('path ~= "**/%s"'):format(c) }
  for _, p in ipairs(parts) do
    containers[#containers + 1] = ('path ~= "**/%s%s"'):format(c, p)
  end
  keep(containers)

  -- Staves.
  local staves = {}
  for _, p in ipairs(parts) do
    staves[#staves + 1] = ('path ~= "**/%s%s/stave*"'):format(c, p)
  end
  keep(staves)

  -- Their non-air leaves.
  local leaves = {}
  for _, p in ipairs(parts) do
    leaves[#leaves + 1] =
      ('path ~= "**/%s%s/stave*/**" && is_leaf && material != "Air"'):format(c, p)
  end
  keep(leaves)
end

-- ── Rules ────────────────────────────────────────────────────────────────────

rule {
  match = 'any(path ~= "**/ECal", path ~= "**/HCal")',
  tessellation = { skip_geometry = true },
}

rule {
  match = 'any(path ~= "**/ECalBarrel", path ~= "**/ECalEndcap_A", path ~= "**/ECalEndcap_B", ' ..
          'path ~= "**/HCalBarrel", path ~= "**/HCalEndcap_A", path ~= "**/HCalEndcap_B")',
  tessellation = { skip_geometry = true },
}

local staves = {}
for _, c in ipairs(calos) do
  for _, p in ipairs(parts) do
    staves[#staves + 1] = ('path ~= "**/%s%s/stave*"'):format(c, p)
  end
end
rule {
  match = staves,
  tessellation = {
    merge_descendants = true,
    -- Band-limit the cycling absorber/scintillator layer colors so the stack
    -- doesn't alias into moire at distance (viewer material-stack prefilter).
    average_material_stack = true,
    max_segments_circle = 10,
    fallback = "fail",
  },
}
