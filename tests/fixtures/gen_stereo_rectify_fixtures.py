#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Generate stereo_rectify_fixtures.h: OpenCV's answers for u_stereo_rectify.

ADR-043 R2. The runtime's rectifier (src/xrt/auxiliary/util/u_stereo_rectify.c)
has NO OpenCV dependency; these fixtures pin it to OpenCV's
stereoRectify(flags=CALIB_ZERO_DISPARITY, alpha=0) + initUndistortRectifyMap.
Run offline (needs opencv-python + numpy) and commit the header:

    python3 tests/fixtures/gen_stereo_rectify_fixtures.py > tests/fixtures/stereo_rectify_fixtures.h

Case "sim_distorted" mirrors sim_stereo_camera_truth_init() in
src/xrt/drivers/sim_display/sim_display_stereo_camera_pattern.c — keep the two
in sync (tests_stereo_rectify also checks the C truth against these inputs).
"""
import math

import cv2
import numpy as np


def sim_truth(w, h):
    ideal_fx = (w / 2.0) / math.tan(math.radians(34.0))
    icx, icy = (w - 1) * 0.5, (h - 1) * 0.5
    K1 = np.array([[ideal_fx * 1.012, 0, icx + w * 0.009], [0, ideal_fx * 1.008, icy - h * 0.008], [0, 0, 1]])
    K2 = np.array([[ideal_fx * 0.991, 0, icx - w * 0.007], [0, ideal_fx * 0.994, icy + h * 0.007], [0, 0, 1]])
    D1 = np.array([-0.21, 0.06, 0.0008, -0.0006, -0.005, 0, 0, 0])
    D2 = np.array([-0.17, 0.04, -0.0005, 0.0007, 0.0, 0, 0, 0])
    S, _ = cv2.Rodrigues(np.radians(np.array([0.35, 0.25, 0.45])))
    R = S @ S
    T = -S @ np.array([50.0, 0, 0])
    return dict(name="sim_distorted", w=w, h=h, cw=0, ch=0, model=1, K=[K1, K2], D=[D1, D2], R=R, T=T)


def sr_like():
    K1 = np.array([[421.7, 0, 318.2], [0, 422.9, 243.6], [0, 0, 1]])
    K2 = np.array([[424.1, 0, 322.9], [0, 425.0, 238.1], [0, 0, 1]])
    D1 = np.array([-0.082, 0.121, 0.0011, -0.0004, -0.061, 0, 0, 0])
    D2 = np.array([-0.075, 0.098, -0.0007, 0.0009, -0.044, 0, 0, 0])
    R, _ = cv2.Rodrigues(np.array([0.0021, -0.0043, 0.0032]))
    T = np.array([-63.47, 0.41, -0.83])
    return dict(name="sr_like", w=640, h=480, cw=0, ch=0, model=1, K=[K1, K2], D=[D1, D2], R=R, T=T)


def radtan8_scaled():
    # Calibrated at 1280x960, frames at 640x480: the rectifier rescales K.
    K1c = np.array([[905.0, 0, 641.5], [0, 903.0, 477.0], [0, 0, 1]])
    K2c = np.array([[899.0, 0, 636.0], [0, 900.5, 483.5], [0, 0, 1]])
    D1 = np.array([0.21, -0.05, 0.0006, 0.0003, 0.01, 0.43, 0.02, 0.004])
    D2 = np.array([0.19, -0.04, -0.0004, 0.0005, 0.008, 0.40, 0.015, 0.003])
    R, _ = cv2.Rodrigues(np.array([-0.0035, 0.0061, -0.0018]))
    T = np.array([-40.2, -0.35, 0.52])

    def scale(K, s):
        K = K.copy()
        K[0, 0] *= s
        K[1, 1] *= s
        K[0, 2] = (K[0, 2] + 0.5) * s - 0.5
        K[1, 2] = (K[1, 2] + 0.5) * s - 0.5
        return K

    return dict(name="radtan8_scaled", w=640, h=480, cw=1280, ch=960, model=2, K=[K1c, K2c], D=[D1, D2], R=R,
                T=T, Kframe=[scale(K1c, 0.5), scale(K2c, 0.5)])


def converged_cc(Kf, D, R1, R2, w, h):
    """OpenCV's principal-point rule with undistortPoints run to convergence.

    cvStereoRectify undistorts the four corners with only 5 fixed-point
    iterations, which leaves a strong lens tenths of a pixel (or more) short;
    the runtime iterates to convergence. This is the converged reference.
    """
    fc = 1e300
    for K, d in zip(Kf, D):
        f = K[1, 1]
        if d[0] < 0:
            f *= 1 + d[0] * (w * w + h * h) / (4 * f * f)
        fc = min(fc, f)
    corners = np.array([[[0, 0]], [[w - 1, 0]], [[0, h - 1]], [[w - 1, h - 1]]], np.float64)
    cc = []
    for K, d, Rr in zip(Kf, D, (R1, R2)):
        u = cv2.undistortPointsIter(corners, K, d, None, None,
                                    (cv2.TERM_CRITERIA_COUNT | cv2.TERM_CRITERIA_EPS, 500, 1e-15)).reshape(-1, 2)
        q = (Rr @ np.hstack([u, np.ones((4, 1))]).T).T
        avg = (q[:, :2] / q[:, 2:3]).mean(axis=0)
        cc.append(((w - 1) / 2 - fc * avg[0], (h - 1) / 2 - fc * avg[1]))
    return fc, (cc[0][0] + cc[1][0]) / 2, (cc[0][1] + cc[1][1]) / 2


def arr(a):
    return "{" + ", ".join("%.17g" % float(x) for x in np.asarray(a).ravel()) + "}"


GRID_X, GRID_Y = 7, 5


def emit(c):
    w, h = c["w"], c["h"]
    Kf = c.get("Kframe", c["K"])
    D = [d if c["model"] == 2 else d[:5] for d in c["D"]]
    R1, R2, P1, P2, Q, roi1, roi2 = cv2.stereoRectify(Kf[0], D[0], Kf[1], D[1], (w, h), c["R"], c["T"],
                                                      flags=cv2.CALIB_ZERO_DISPARITY, alpha=0)
    fc, ccx, ccy = converged_cc(Kf, D, R1, R2, w, h)
    samples = []
    for e, (Rr, P) in enumerate(((R1, P1), (R2, P2))):
        mx, my = cv2.initUndistortRectifyMap(Kf[e], D[e], Rr, P, (w, h), cv2.CV_32FC1)
        for j in range(GRID_Y):
            for i in range(GRID_X):
                u = round(i * (w - 1) / (GRID_X - 1))
                v = round(j * (h - 1) / (GRID_Y - 1))
                samples += [e, u, v, float(mx[v, u]), float(my[v, u])]
    n = c["name"]
    print("static const struct stereo_rectify_fixture k_fixture_%s = {" % n)
    print('\t"%s", %d, %d, %d, %d, %d,' % (n, w, h, c["cw"], c["ch"], c["model"]))
    print("\t{%s, %s}," % (arr(c["K"][0]), arr(c["K"][1])))
    print("\t{%s, %s}," % (arr(c["D"][0]), arr(c["D"][1])))
    print("\t%s, %s," % (arr(c["R"]), arr(c["T"])))
    print("\t%s, %s, %s, %s," % (arr(R1), arr(R2), arr(P1), arr(P2)))
    print("\t%s," % arr([fc, ccx, ccy]))
    print("\t%s," % arr(samples))
    print("};\n")


def main():
    print("// Copyright 2026, The DisplayXR Project")
    print("// SPDX-License-Identifier: BSL-1.0")
    print("// GENERATED by gen_stereo_rectify_fixtures.py with OpenCV %s — do not edit." % cv2.__version__)
    print("// OpenCV stereoRectify(CALIB_ZERO_DISPARITY, alpha=0) + initUndistortRectifyMap samples.")
    print("#pragma once\n")
    print("#define STEREO_RECTIFY_FIXTURE_SAMPLES (2 * %d * %d)\n" % (GRID_X, GRID_Y))
    print("struct stereo_rectify_fixture\n{")
    print("\tconst char *name;\n\tint width, height, calib_width, calib_height, model;")
    print("\tdouble K[2][9]; //!< as calibrated (calib size when set)\n\tdouble D[2][8];")
    print("\tdouble R[9], T[3];")
    print("\tdouble R1[9], R2[9], P1[12], P2[12]; //!< OpenCV's answer")
    print("\tdouble converged[3]; //!< pre-crop focal, cx, cy with converged undistortPoints")
    print("\tdouble samples[STEREO_RECTIFY_FIXTURE_SAMPLES * 5]; //!< eye, u, v, map_x, map_y (OpenCV R/P)")
    print("};\n")
    for c in (sim_truth(640, 480), sr_like(), radtan8_scaled()):
        emit(c)


if __name__ == "__main__":
    main()
