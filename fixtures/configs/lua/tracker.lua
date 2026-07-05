-- Tracker fragment: the convention (irregular container / grouping / endcap
-- names) is encoded once; the subsystems are listed as data. Adding a fourth
-- subsystem is one line rather than ~30 across three copy-pasted sections.
local subsystems = {
  { plural = "Pixels",      layer = "PixelLayer",      endcap = "Pixel" },
  { plural = "ShortStrips", layer = "ShortStripLayer", endcap = "ShortStrip" },
  { plural = "LongStrips",  layer = "LongStripLayer",  endcap = "LongStrip" },
}

local function path(g) return 'path ~= "' .. g .. '"' end
local function leaf(g) return path(g) .. ' && is_leaf' end

for _, s in ipairs(subsystems) do
  local eN, eP = s.endcap .. "EndcapN", s.endcap .. "EndcapP"

  -- keep the container spine and its sensitive leaves
  keep {
    path("**/" .. s.plural),
    path("**/" .. s.plural .. "/*Barrel"),
    path("**/" .. s.plural .. "/*EndcapN"),
    path("**/" .. s.plural .. "/*EndcapP"),
    path("**/" .. s.layer .. "*/stave*"),
    leaf("**/" .. s.layer .. "*/stave*/**"),
    path("**/" .. eN .. "*/disk*"), leaf("**/" .. eN .. "*/disk*/**"),
    path("**/" .. eP .. "*/disk*"), leaf("**/" .. eP .. "*/disk*/**"),
  }

  -- structural container nodes carry no geometry
  rule {
    match = {
      path("**/" .. s.plural),
      path("**/" .. s.plural .. "/*Barrel"),
      path("**/" .. s.layer .. "*"),
      path("**/" .. s.plural .. "/*EndcapN"),
      path("**/" .. s.plural .. "/*EndcapP"),
    },
    tessellation = { skip_geometry = true },
  }

  -- stave / disk groups: merge descendants into one mesh
  rule {
    match = {
      path("**/" .. s.layer .. "*/stave*"),
      path("**/" .. eN .. "*/disk*"),
      path("**/" .. eP .. "*/disk*"),
    },
    tessellation = { merge_descendants = true, max_segments_circle = 10, fallback = "fail" },
  }
end
