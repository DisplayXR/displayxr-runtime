// One-off utility: rotate an .spz file in place using Niantic's coord-system
// converter. Given input/output paths and a target axis flip, calls the spz
// library's convertCoordinates() and rewrites the file.
//
// Usage:
//   spz_rotate <input.spz> <output.spz> <fwd180|y180|x180>
//
// fwd180  = 180° around forward (Z) axis — flips X, Y. Use for assets that
//           come in spinned-in-plane (the butterfly case).
// y180    = 180° around up axis — flips X, Z. Same as a yaw of 180°.
// x180    = 180° around right axis — flips Y, Z. Use for upside-down assets.
//
// All rotations operate in RUB (Niantic native) frame.

#include "load-spz.h"
#include "splat-types.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr,
            "Usage: %s <input.spz> <output.spz> <fwd180|y180|x180>\n", argv[0]);
        return 1;
    }
    const char* inPath = argv[1];
    const char* outPath = argv[2];
    const char* mode = argv[3];

    // RUB target frame for the in-memory cloud; rotations are flag-encoded
    // coord-system targets relative to RUB.
    spz::CoordinateSystem to = spz::CoordinateSystem::UNSPECIFIED;
    if (strcmp(mode, "fwd180") == 0) {
        to = spz::CoordinateSystem::LDB;  // flip X & Y
    } else if (strcmp(mode, "y180") == 0) {
        to = spz::CoordinateSystem::LUF;  // flip X & Z
    } else if (strcmp(mode, "x180") == 0) {
        to = spz::CoordinateSystem::RDF;  // flip Y & Z
    } else {
        fprintf(stderr, "spz_rotate: unknown mode '%s'\n", mode);
        return 1;
    }

    spz::UnpackOptions loadOpts;
    loadOpts.to = spz::CoordinateSystem::RUB;
    spz::GaussianCloud cloud = spz::loadSpz(inPath, loadOpts);
    if (cloud.numPoints == 0) {
        fprintf(stderr, "spz_rotate: failed to load %s\n", inPath);
        return 1;
    }
    printf("spz_rotate: loaded %d gaussians from %s\n", cloud.numPoints, inPath);

    cloud.convertCoordinates(spz::CoordinateSystem::RUB, to);
    printf("spz_rotate: applied %s\n", mode);

    // Save with from=RUB even though we just converted to `to`. This is the
    // trick that turns a coord-system relabel into a persistent rotation:
    // the packer trusts our claim of RUB and packs the (now-rotated) values
    // verbatim; subsequent loads with to=RUB return the rotated data.
    spz::PackOptions saveOpts;
    saveOpts.from = spz::CoordinateSystem::RUB;
    if (!spz::saveSpz(cloud, saveOpts, outPath)) {
        fprintf(stderr, "spz_rotate: failed to save %s\n", outPath);
        return 1;
    }
    printf("spz_rotate: wrote %d gaussians to %s\n", cloud.numPoints, outPath);
    return 0;
}
