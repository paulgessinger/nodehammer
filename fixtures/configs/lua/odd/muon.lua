-- ODD muon spectrometer
--
-- Note the capitalisation: the muon endcaps are spelled `EndCap` here, unlike
-- the tracker's `Endcap`. The predicates match the source geometry, so the two
-- spellings are deliberate rather than a typo.

local endcaps = { "MuonSpectrometerEndCapN", "MuonSpectrometerEndCapP" }

-- ── Selection ────────────────────────────────────────────────────────────────

keep {
  'path ~= "**/MuonSpectrometerBarrel"',
  'path ~= "**/MuonSpectrometerBarrel/chamber*"',
  'path ~= "**/MuonSpectrometerEndCapN"',
  'path ~= "**/MuonSpectrometerEndCapN/layer*"',
  'path ~= "**/MuonSpectrometerEndCapN/layer*/chamber*"',
  'path ~= "**/MuonSpectrometerEndCapP"',
  'path ~= "**/MuonSpectrometerEndCapP/layer*"',
  'path ~= "**/MuonSpectrometerEndCapP/layer*/chamber*"',
}

-- Chamber leaves, minus the gas volumes and the air they sit in.
local leaf_filter = 'is_leaf && material != "Air" && material != "ArCO2"'
keep {
  ('path ~= "**/MuonSpectrometerBarrel/chamber*/**" && %s'):format(leaf_filter),
  ('path ~= "**/%s/layer*/chamber*/**" && %s'):format(endcaps[1], leaf_filter),
  ('path ~= "**/%s/layer*/chamber*/**" && %s'):format(endcaps[2], leaf_filter),
}

-- ── Rules ────────────────────────────────────────────────────────────────────

rule {
  match = [[any(
  path ~= "**/MuonSpectrometerBarrel",
  path ~= "**/MuonSpectrometerEndCapN",
  path ~= "**/MuonSpectrometerEndCapN/layer*",
  path ~= "**/MuonSpectrometerEndCapP",
  path ~= "**/MuonSpectrometerEndCapP/layer*",
)]],
  tessellation = { skip_geometry = true },
}

rule {
  match = {
    'path ~= "**/MuonSpectrometerBarrel/chamber*"',
    ('path ~= "**/%s/layer*/chamber*"'):format(endcaps[1]),
    ('path ~= "**/%s/layer*/chamber*"'):format(endcaps[2]),
  },
  tessellation = { merge_descendants = true, max_segments_circle = 10 },
}
