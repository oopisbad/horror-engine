"""Right-handed rotations. Euler fields use Rz * Ry * Rx, angles in degrees."""

import math


def axis_angle(axis, degrees):
    half = math.radians(degrees) / 2
    return [*(v * math.sin(half) for v in axis), math.cos(half)]


def multiply(a, b):
    x, y, z, w = a
    i, j, k, s = b
    return [
        w * i + x * s + y * k - z * j,
        w * j + y * s + z * i - x * k,
        w * k + z * s + x * j - y * i,
        w * s - x * i - y * j - z * k,
    ]


AXES = ((1, 0, 0), (0, 1, 0), (0, 0, 1))


def quaternion(angles):
    x, y, z = [axis_angle(axis, angle) for axis, angle in zip(AXES, angles)]
    return multiply(z, multiply(y, x))


def angles(q):
    x, y, z, w = q
    sy = max(-1, min(1, 2 * (w * y - z * x)))
    if abs(sy) > 1 - 1e-12:
        return [0, math.copysign(90, sy), math.degrees(2 * math.atan2(z, w))]
    return list(
        map(
            math.degrees,
            (
                math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)),
                math.asin(sy),
                math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)),
            ),
        )
    )


def apply(q, v):
    return multiply(multiply(q, [*v, 0]), [-q[0], -q[1], -q[2], q[3]])[:3]


def orientation(obj):
    return obj.get("rotation", [0, obj.get("yaw", 0), 0])
