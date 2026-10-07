#pragma once

// The geometry both halves of the golden comparison import.
//
// Shared fixture: repeated placements, rotations, a tube, and a transformed
// boolean operand. World-transform composition is checked separately in core.cpp.

#include <TGeoBBox.h>
#include <TGeoCompositeShape.h>
#include <TGeoManager.h>
#include <TGeoMaterial.h>
#include <TGeoMatrix.h>
#include <TGeoMedium.h>
#include <TGeoTube.h>
#include <TGeoVolume.h>

/// Build the shared fixture. The caller owns the manager.
inline TGeoManager *makeGoldenGeometry() {
    auto *mgr = new TGeoManager("golden", "golden equivalence fixture");

    auto *vacuum = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *medium = new TGeoMedium("vacuum", 1, vacuum);

    auto *top = mgr->MakeBox("world", medium, 500.0, 500.0, 500.0);
    mgr->SetTopVolume(top);

    // Placed twice, at different rotations, from one logical volume.
    auto *plate = mgr->MakeBox("plate", medium, 40.0, 12.5, 3.0);
    top->AddNode(plate, 1,
                 new TGeoCombiTrans(10.0, 20.0, 30.0, new TGeoRotation("r1", 31.0, 17.0, 5.0)));
    top->AddNode(
        plate, 2,
        new TGeoCombiTrans(-45.5, 8.25, -12.75, new TGeoRotation("r2", -12.0, 64.0, 23.5)));

    // A tube, so the dispatch covers a shape with radii and a phi range.
    auto *pipe = mgr->MakeTubs("pipe", medium, 15.0, 22.5, 90.0, 12.0, 250.0);
    top->AddNode(pipe, 1,
                 new TGeoCombiTrans(0.0, 0.0, 120.0, new TGeoRotation("r3", 7.5, 3.25, 0.0)));

    // A boolean, whose right operand carries its own transform -- the field the
    // Semantic IR stores as a Mat4 and the codec interns into the pool.
    auto *outer = new TGeoBBox("outer", 30.0, 30.0, 30.0);
    auto *inner = new TGeoBBox("inner", 20.0, 20.0, 40.0);
    auto *shift = new TGeoCombiTrans(2.5, -1.5, 0.0, new TGeoRotation("r4", 45.0, 0.0, 0.0));
    shift->SetName("shift");
    shift->RegisterYourself();
    auto *cut = new TGeoCompositeShape("cut", "outer - inner:shift");
    auto *cutVol = new TGeoVolume("cutVol", cut, medium);
    top->AddNode(cutVol, 1, new TGeoTranslation(-100.0, 0.0, 0.0));

    mgr->CloseGeometry();
    return mgr;
}
