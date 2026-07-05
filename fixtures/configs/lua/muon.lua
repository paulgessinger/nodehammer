-- ODD muon spectrometer: a barrel plus two endcaps. The barrel holds chambers
-- directly; each endcap nests them one level deeper, under layers. That small
-- asymmetry is exactly the kind of thing worth encoding once.
local barrel = "**/MuonSpectrometerBarrel"
local endcaps = { "**/MuonSpectrometerEndCapN", "**/MuonSpectrometerEndCapP" }

local function p(g) return 'path ~= "' .. g .. '"' end
-- sensitive chamber leaf: real material, not the air/gas volumes
local function sens(g) return p(g) .. ' && is_leaf && material != "Air" && material != "ArCO2"' end

-- keep the container spine
local spine = { p(barrel), p(barrel .. "/chamber*") }
for _, e in ipairs(endcaps) do
  spine[#spine + 1] = p(e)
  spine[#spine + 1] = p(e .. "/layer*")
  spine[#spine + 1] = p(e .. "/layer*/chamber*")
end
keep(spine)

-- keep the sensitive chamber leaves
local leaves = { sens(barrel .. "/chamber*/**") }
for _, e in ipairs(endcaps) do
  leaves[#leaves + 1] = sens(e .. "/layer*/chamber*/**")
end
keep(leaves)

-- structural containers carry no geometry
local containers = { p(barrel) }
for _, e in ipairs(endcaps) do
  containers[#containers + 1] = p(e)
  containers[#containers + 1] = p(e .. "/layer*")
end
rule { match = containers, tessellation = { skip_geometry = true } }

-- chamber groups: merge descendants into one mesh
local chambers = { p(barrel .. "/chamber*") }
for _, e in ipairs(endcaps) do
  chambers[#chambers + 1] = p(e .. "/layer*/chamber*")
end
rule { match = chambers, tessellation = { merge_descendants = true, max_segments_circle = 10 } }
