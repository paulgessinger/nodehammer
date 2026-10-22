-- Shared authoring for the inspected nominal-standard and Phase-2 snapshots.
-- Placement names are often ANON: use original paths and source materials.
local apply_materials = use("materials.lua")
return function(phase, detailed)
  config { hoist_orphans = true, deduplicate_shapes = true }
  export("gltf", { unit_scale = 0.001, bake_unit_scale = true }) -- GeoModel mm -> m
  export("obj", { unit_scale = 0.001 })
  defaults {
    tessellation = { max_segments_circle = detailed and 24 or 48,
                     generic_trap_tolerance = detailed and 0.1 or 1.0,
                     fallback = "skip" },
    extras = { visible = true },
  }
  local palette = {
    {"support", "#8795A1"}, {"pixel", "#57C7E3"}, {"strip", "#537FE0"},
    {"trt", "#B79ADC"}, {"hgtd", "#ED79BA"}, {"lar", "#E4B649"},
    {"tile", "#D87538"}, {"muon", "#65B69C"}, {"pipe", "#C4C9D0"},
    {"copper", "#B97450"}, {"absorber", "#8E789E"},
    {"magnet_support", "#C69961"},
  }
  for _, p in ipairs(palette) do
    material(p[1], { base_color = p[2], metallic = 0.15, roughness = 0.65 })
  end
  rule { material = "support" }
  local systems = {{"BeamPipe", "pipe"}, {"LArBarrel", "lar"},
    {"LArEndcapPos", "lar"}, {"LArEndcapNeg", "lar"},
    {"Tile", "tile"}, {"Muon", "muon"}}
  if phase == 1 then
    for _, s in ipairs({{"Pixel", "pixel"}, {"SCT", "strip"}, {"TRT", "trt"}}) do
      systems[#systems+1] = s
    end
  else
    for _, s in ipairs({{"ITkPixel", "pixel"}, {"ITkStrip", "strip"}, {"HGTD", "hgtd"}}) do
      systems[#systems+1] = s
    end
  end
  drop 'true'
  for _, s in ipairs(systems) do
    local predicate = ('path ~= "/WorldLog/%s" || path ~= "/WorldLog/%s/**"'):format(s[1],s[1])
    rule { match = predicate, material = s[2] }
  end

  -- Real envelope solids are explicit calorimeter/chamber proxies. Descendants are
  -- omitted, not merged: no sampling foils, chamber wires or service stacks.
  keep 'path ~= "/WorldLog/BeamPipe/**" && !(material ~= "std::Vacuum" || material ~= "*Air")'
  keep 'path ~= "/WorldLog/Muon/*_station"'
  keep 'path ~= "/WorldLog/Tile/Barrel/ANON" || path ~= "/WorldLog/Tile/EBarrel*/ANON" || path ~= "/WorldLog/Tile/ITC*/ANON" || path ~= "/WorldLog/Tile/Gap*/ANON" || path ~= "/WorldLog/Tile/Crack*/ANON"'
  -- EMB/presampler and EMEC/HEC envelopes; FCAL absorber bodies.
  keep 'path ~= "/WorldLog/LArBarrel/Total LAR Volume/*/ANON" && material ~= "std::LiquidArgon"'
  keep 'path ~= "/WorldLog/LArEndcap*/ANON/ANON" && (material ~= "std::LiquidArgon" || material ~= "LAr::FCal*Absorber")'
  if phase == 1 then
    keep 'path ~= "/WorldLog/Pixel/Barrel/Layer*" || path ~= "/WorldLog/Pixel/EndCap*/Disk*"'
    keep 'path ~= "/WorldLog/SCT/Layer#*" || path ~= "/WorldLog/SCT/Wheel#*"'
    if not detailed then
      keep 'path ~= "/WorldLog/TRT/ANON" && !is_leaf'
    else
      -- Straw/StrawM are the outer straw cylinders; leave their gas and wires
      -- out. Eight-sided tubes preserve every placement at detector-view scale.
      local straws = 'path ~= "/WorldLog/TRT/**" && material == "trt::Straw" && (volume == "Straw" || volume == "StrawM")'
      keep(straws)
      rule { match = straws, tessellation = { max_segments_circle = 8 } }
    end
  else
    -- Keep silicon solids for the irregular ITk layout; no service
    -- material. These retain inclined modules and endcap ring structure.
    keep 'path ~= "/WorldLog/ITkPixel/**" && (material ~= "Silicon*" || material ~= "*Silicon")'
    keep 'path ~= "/WorldLog/ITkStrip/**" && (material ~= "Silicon*" || material ~= "*Silicon" || material ~= "SiMetal*")'
    keep 'path ~= "/WorldLog/HGTD/*/HGTDModuleLayer*"'
  end
  if detailed then
    -- Preserve silicon solids (including silicon electronics), but omit
    -- calorimeter sampling foils and chamber gas/wire internals.
    if phase == 1 then
      drop 'path ~= "/WorldLog/Pixel/**" || path ~= "/WorldLog/SCT/**"'
      keep '(path ~= "/WorldLog/Pixel/**" || path ~= "/WorldLog/SCT/**") && (material ~= "Silicon*" || material ~= "*Silicon")'
    else
      drop 'path ~= "/WorldLog/HGTD/**"'
      keep 'path ~= "/WorldLog/HGTD/**" && (material ~= "Silicon*" || material ~= "*Silicon")'
    end
    -- Only these structural solids, never entire support/service subtrees.
    -- Names were inspected in the two input snapshots. Detailed finishes are
    -- assigned from source materials by materials.lua below.
    local function supports(system, volumes)
      local matches = {}
      for _, volume in ipairs(volumes) do
        matches[#matches+1] = ('volume ~= "%s"'):format(volume)
      end
      local predicate = ('path ~= "/WorldLog/%s/**" && (%s) && !(material ~= "*Air" || material == "special::Ether" || material == "std::Vacuum")'):format(system, table.concat(matches, " || "))
      keep(predicate)
      rule { match = predicate, material = "support", tessellation = { max_segments_circle = 128 } }
    end
    if phase == 1 then
      supports("Pixel", {"Brl*_HalfShell", "Brl*_StaveRing", "Brl*_Ring", "Brl*_FrameCone", "DiskSup*", "Omega", "FacePlate", "EndBlock"})
      supports("SCT", {"SupportCyl", "DiscSupport*", "SupportFrame", "BaseBoard", "SCT_FwdSpine*", "SCT_FwdSubSpine*", "DiscFixation"})
      -- ModuleShell* are material-filled mothers containing the straw region.
      -- Their unsubtracted solids overlap the explicitly rendered straws.
      supports("TRT", {"BarrelInnerSupport", "BarrelOuterSupport", "InnerSupportA", "InnerSupportB", "OuterSupportA", "OuterSupportB"})
    else
      supports("ITkPixel", {"InnerPixBarrelSupport_*", "InnerPixEndcap_*RingSupport", "InclL*HalfShell", "InclL*Support", "L*HalfShell", "L*HalfRingFaceSheet", "L*HalfRingCarbonFoam*", "L4endcapFaceSheet", "L4endcap*CarbonFoam"})
      -- GeoModel material-filled mothers are not daughter-subtracted by
      -- the renderer: StaveCore shares faces with its end closeout;
      -- PetalCore encloses the face sheets and shares their radial faces.
      -- Keep the explicit skins/channels/closeouts, not those bulk mothers.
      supports("ITkStrip", {"StaveFacesheetAll", "StaveCchannel*", "StaveCylHat*", "PetalFaceSheetAll", "Flange*", "StaveCloseout*", "StaveMount*", "PetalCloseout*"})
      supports("HGTD", {"HGTDSupportPlate"})
    end
    -- Smooth the inner tracker rings and beam pipe without multiplying the
    -- cost of the 298k TRT straws. Boxes/trapezoids ignore circle resolution.
    rule {
      match = 'path ~= "/WorldLog/BeamPipe/**" || path ~= "/WorldLog/Pixel/**" || path ~= "/WorldLog/SCT/**" || path ~= "/WorldLog/ITkPixel/**" || path ~= "/WorldLog/ITkStrip/**" || path ~= "/WorldLog/HGTD/**"',
      tessellation = { max_segments_circle = 128 },
    }
    -- Close-up quality for circular backing structures, without changing
    -- physical sensor/module segmentation or the repeated straw resolution.
    rule {
      match = '(path ~= "/WorldLog/Pixel/**" || path ~= "/WorldLog/SCT/**" || path ~= "/WorldLog/ITkPixel/**" || path ~= "/WorldLog/ITkStrip/**" || path ~= "/WorldLog/HGTD/**") && (volume ~= "*Ring*" || volume ~= "*ring*" || volume ~= "*Disk*" || volume ~= "*Disc*" || volume ~= "*SupportCyl*" || volume ~= "*HalfShell*" || volume ~= "*Flange*" || volume ~= "*SupportPlate*")',
      tessellation = { max_segments_circle = 256 },
    }
    -- Retain real cryostat shells alongside the calorimeter envelopes.
    keep 'path ~= "/WorldLog/LAr*/**" && volume ~= "*Cryostat*" && !(material ~= "*Air" || material ~= "std::Vacuum" || material ~= "std::LiquidArgon" || material ~= "special::Ether")'
  end
  -- Toroids live in anonymous passive Muon assemblies (ECT_Toroids and
  -- BAR_Toroid logical volumes). Retain the complete passive assemblies so
  -- their coil/support descendants remain present in every scenario.
  -- Keep their actual solids, never the enclosing air/ether proxy volumes.
  local passive_muon = 'path ~= "/WorldLog/Muon/ANON" || path ~= "/WorldLog/Muon/ANON/**"'
  keep(passive_muon)
  rule { match = passive_muon, material = "magnet_support" }
  -- NSW chambers are assembly proxies, like the other muon stations.
  -- Their material-filled mothers share outlines with gas/board/frame
  -- daughters. Rendering the full stack produces coincident edge faces.
  drop('(' .. passive_muon .. ') && ((volume ~= "sTGC*" && material != "muo::Honeycomb") || volume == "MM_Sensitive" || volume == "MM_Frame")')
  rule {
    match = '(' .. passive_muon .. ') && (material ~= "*Air" || material ~= "special::Ether" || material ~= "std::Vacuum")',
    tessellation = { skip_geometry = true },
  }
  rule { match = 'path ~= "/WorldLog"', tessellation = { skip_geometry = true } }
  if detailed then
    apply_materials()
  end
end
