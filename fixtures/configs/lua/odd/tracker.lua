-- ODD tracker: pixels, short strips, long strips
--
-- The three subsystems are structurally identical -- barrel layers holding
-- staves, endcap disks holding rings -- so the predicate lists are generated
-- from the subsystem name rather than written out three times.

local subsystems = {
  -- plural (the container node)  singular (the layer/endcap prefix)
  { "Pixels", "Pixel" },
  { "ShortStrips", "ShortStrip" },
  { "LongStrips", "LongStrip" },
}

-- ── Selection ────────────────────────────────────────────────────────────────

local containers = {}
for _, s in ipairs(subsystems) do
  local plural = s[1]
  containers[#containers + 1] = ('path ~= "**/%s"'):format(plural)
  containers[#containers + 1] = ('path ~= "**/%s/*Barrel"'):format(plural)
  containers[#containers + 1] = ('path ~= "**/%s/*EndcapN"'):format(plural)
  containers[#containers + 1] = ('path ~= "**/%s/*EndcapP"'):format(plural)
end
keep(containers)

-- Keep stave/disk nodes and all their leaf descendants, one block per subsystem.
for _, s in ipairs(subsystems) do
  local sing = s[2]
  keep {
    ('path ~= "**/%sLayer*"'):format(sing),
    ('path ~= "**/%sLayer*/stave*"'):format(sing),
    ('path ~= "**/%sLayer*/stave*/**" && is_leaf'):format(sing),
    ('path ~= "**/%sEndcapN*/disk*"'):format(sing),
    ('path ~= "**/%sEndcapN*/disk*/**" && is_leaf'):format(sing),
    ('path ~= "**/%sEndcapP*/disk*"'):format(sing),
    ('path ~= "**/%sEndcapP*/disk*/**" && is_leaf'):format(sing),
  }
end

-- ── Rules ────────────────────────────────────────────────────────────────────

-- Structural nodes: no geometry.
local structural = {}
for _, s in ipairs(subsystems) do
  local plural, sing = s[1], s[2]
  structural[#structural + 1] = ('path ~= "**/%s"'):format(plural)
  structural[#structural + 1] = ('path ~= "**/%s/*Barrel"'):format(plural)
  structural[#structural + 1] = ('path ~= "**/%sLayer*"'):format(sing)
  structural[#structural + 1] = ('path ~= "**/%s/*EndcapN"'):format(plural)
  structural[#structural + 1] = ('path ~= "**/%s/*EndcapP"'):format(plural)
end
rule { match = structural, tessellation = { skip_geometry = true } }

-- Barrel/endcap/stave nodes: merge descendants. Written out rather than
-- generated -- the long-strip group lists its endcaps before its layers, and
-- the order is preserved here so this stays a transcription of odd/tracker.toml.
rule {
  match = {
    'path ~= "**/PixelLayer*/stave*"',
    'path ~= "**/PixelEndcapN*/disk*"',
    'path ~= "**/PixelEndcapP*/disk*"',
    'path ~= "**/ShortStripLayer*/stave*"',
    'path ~= "**/ShortStripEndcapN*/disk*"',
    'path ~= "**/ShortStripEndcapP*/disk*"',
    'path ~= "**/LongStripEndcapN*/disk*"',
    'path ~= "**/LongStripEndcapP*/disk*"',
    'path ~= "**/LongStripLayer*/stave*"',
  },
  tessellation = { merge_descendants = true, max_segments_circle = 10, fallback = "fail" },
}

-- @TODO: Add SupportCylinder0_0...

rule {
  match = {
    'path ~= "**/PixelEndcap*/**/DiskSupport_*"',
    'path ~= "**/*StripEndcap*/**/RingSupport*"',
  },
  tessellation = { max_segments_circle = 48 },
}
