-- Materials fragment: three render materials plus a table-driven
-- source-material -> render-material map. The alias case (spelling variants)
-- falls out of the same code path that handles a single source glob.
local K = use("lib/constants.lua")

material("silicon", { base_color = K.palette.silicon, metallic = 1.0, roughness = 0.05 })
material("kapton",  { base_color = K.palette.kapton })
material("copper",  { base_color = K.palette.copper, metallic = 1.0, roughness = 0.2 })

-- source-material glob(s)  ->  render material
local material_map = {
  { "Beryllium",                        "silicon" },
  { "Silicon",                          "silicon" },
  { "Kapton",                           "kapton" },
  { "Aluminum",                         "copper" },
  { "CarbonFiber*",                     "silicon" },
  { "Titanium",                         "copper" },
  { "CarbonFoam",                       "silicon" },
  { "Copper",                           "copper" },
  { "TungstenDens24",                   "copper" },
  { "G10",                              "kapton" },
  { {"GroundOrHvMix", "GroundOrHVMix"}, "copper" },  -- spelling aliases -> OR
  { {"SiPcbMix", "siPCBMix"},           "kapton" },
  { "Steel*",                           "copper" },
  { "Polystyrene",                      "kapton" },
}

local function match_material(src)          -- normalize scalar-or-list -> OR list
  local srcs = type(src) == "table" and src or { src }
  local out = {}
  for _, s in ipairs(srcs) do out[#out + 1] = ('material ~= "%s"'):format(s) end
  return out
end

for _, m in ipairs(material_map) do
  rule { match = match_material(m[1]), material = m[2] }
end
