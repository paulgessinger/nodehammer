-- ODD calorimeters: ECal + HCal, each a Barrel plus two Endcaps. The per-calo
-- structure (container spine → staves → sensitive leaves) is identical, so
-- encode it once and list the calorimeters as data.
local calos = { "ECal", "HCal" }
local groups = { "Barrel", "Endcap_A", "Endcap_B" } -- suffixes under each calo

local function p(g) return 'path ~= "' .. g .. '"' end
local function leaf(g) return p(g) .. ' && is_leaf && material != "Air"' end

local containers, grouping, staves, staveLeaves = {}, {}, {}, {}

for _, c in ipairs(calos) do
  -- keep the calo container and its Barrel/Endcap grouping nodes
  local spine = { p("**/" .. c) }
  for _, g in ipairs(groups) do
    spine[#spine + 1] = p("**/" .. c .. g)
  end
  keep(spine)

  containers[#containers + 1] = p("**/" .. c)
  for _, g in ipairs(groups) do
    local grp = c .. g -- e.g. ECalBarrel, HCalEndcap_A
    grouping[#grouping + 1] = p("**/" .. grp)
    staves[#staves + 1] = p("**/" .. grp .. "/stave*")
    staveLeaves[#staveLeaves + 1] = leaf("**/" .. grp .. "/stave*/**")
  end
end

keep(staves)      -- stave grouping nodes
keep(staveLeaves) -- sensitive leaves (everything but Air)

-- structural containers carry no geometry
rule { match = containers, tessellation = { skip_geometry = true } }
rule { match = grouping, tessellation = { skip_geometry = true } }

-- stave groups: merge descendants into one mesh
rule {
  match = staves,
  tessellation = { merge_descendants = true, max_segments_circle = 10, fallback = "fail" },
}
