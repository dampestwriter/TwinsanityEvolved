#pragma once

#include <array>
#include <cstdint>

namespace title {

// The culling VU0 set's particle block view (micro entries 0xA08 and 0xB90; TR include/platform/math.h ParticleBlockView)
// as value arithmetic. The view record the game keeps in VU0 data memory (19 quadwords at the view's address): the frustum
// planes in two groups of four lanes -- |n| xyz (qw 0..2 / 7..9), n xyz (3..5 / 10..12), d (6 / 13) -- the camera's place
// (14) and the view's matrix to the screen (15..18). The program answers with the block's matrix to the screen
// (vf29..vf26), the place's distance from the camera (vf30.x) and VI2, the OR of the MAC sign flags of (centre +- radius)
// for both plane groups: non-zero when the box of the extents around the place lies wholly past a plane.
//
// 0xA08 (the emissions carrying their emitter): rows 0..2 = the gravity matrix's rows times the view's matrix; row 3 = the
// place moved TOWARDS THE CAMERA BY `scale` WORLD UNITS (place - unit(place - camera) * scale, w 1) times the view's matrix.
// 0xB90 (keepsTranslation: the particles already hold world positions): rows 0..2 = the view's matrix rows themselves (the
// gravity ignored); row 3 = (-unit(place - camera) * scale, w 1) times the view's matrix -- only the pull, no place.
// Pinned to the retail microprogram on the interpreter by tests/vu/test_vu0_particle_view.cpp (a private fixture).
class ParticleViewMath {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct View {
        std::array<Vector, 19> qw{};   // the 19 quadwords at the view's VU0 address
    };
    struct Result {
        Matrix matrix;      // vf29, vf28, vf27, vf26
        float distance;     // vf30.x
        uint32_t flags;     // VI2: 0 = the box is not wholly past any plane
    };
    // extents (x y z, w 1), gravity rows 0..2 (vf3..vf5), place (x y z, w 1), scale (vf7.x)
    static Result BlockView(const View& view, const Vector& extents, const Matrix& gravity, const Vector& place, float scale,
                            bool keepsTranslation);
};

}  // namespace title
