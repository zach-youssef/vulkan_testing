#!/usr/bin/env python3
"""Writes the scene example's model, assets/scene.gltf and assets/scene.bin: a ground plane, three
boxes and three spheres, built in world space (nothing in the scene moves but the light and the
camera, so the transforms are baked into the vertices, and the nodes have none).

Each vertex's COLOR_0 carries its surface's material, which scene.zgal's shaders read: a linear
albedo (rgb) and a roughness (a). Front faces are counter-clockwise seen from outside, as glTF
requires; the example's projections flip Y for Vulkan, which keeps them counter-clockwise on screen.

Run it from anywhere after changing the scene: python3 examples/scene/make_scene.py
"""

import json
import math
import os
import struct

# A mesh: vertices (position, normal, albedo and roughness) and triangles' indices


class Mesh:
    def __init__(self):
        self.vertices = []
        self.indices = []

    # Adds a vertex transformed by `transform` (a 4x4 row-major matrix). Normals use the inverse
    # transpose, so non-uniform scales keep them perpendicular to the surface.
    def add_vertex(self, transform, position, normal, material):
        p = transform_point(transform, position)
        n = normalize(mat3_mul(normal_matrix(transform), normal))
        self.vertices.append((p, n, material))
        return len(self.vertices) - 1

    # A quad from corner, spanning edges u and v (counter-clockwise from u to v seen from the front)
    def add_quad(self, transform, corner, u, v, material):
        normal = normalize(cross(u, v))
        a = self.add_vertex(transform, corner, normal, material)
        b = self.add_vertex(transform, add(corner, u), normal, material)
        c = self.add_vertex(transform, add(add(corner, u), v), normal, material)
        d = self.add_vertex(transform, add(corner, v), normal, material)
        self.indices += [a, b, c, c, d, a]


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def scale(a, s): return tuple(x * s for x in a)
def neg(a): return tuple(-x for x in a)
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def normalize(a):
    length = math.sqrt(sum(x * x for x in a))
    return tuple(x / length for x in a)


IDENTITY = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def translate(t):
    m = [row[:] for row in IDENTITY]
    for i in range(3):
        m[i][3] = t[i]
    return m


def rotate_y(degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def scaling(s):
    return [[s[0], 0, 0, 0], [0, s[1], 0, 0], [0, 0, s[2], 0], [0, 0, 0, 1]]


def transform_point(m, p):
    return tuple(sum(m[i][k] * (p[k] if k < 3 else 1.0) for k in range(4)) for i in range(3))


def mat3_mul(m, v):
    return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))


def normal_matrix(m):
    # The inverse transpose of the upper 3x3
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    cofactor = [[(a[(i + 1) % 3][(j + 1) % 3] * a[(i + 2) % 3][(j + 2) % 3]
                  - a[(i + 1) % 3][(j + 2) % 3] * a[(i + 2) % 3][(j + 1) % 3]) / det
                 for j in range(3)] for i in range(3)]
    return cofactor   # The inverse's transpose is the cofactor matrix over the determinant


# A square in the XZ plane facing +Y, centred on the origin
def plane(size, material):
    mesh = Mesh()
    h = size * 0.5
    mesh.add_quad(IDENTITY, (-h, 0.0, h), (size, 0.0, 0.0), (0.0, 0.0, -size), material)
    return mesh


# A unit cube centred on the origin, then transformed (scale it for boxes)
def cube(material, transform):
    mesh = Mesh()
    x, y, z = (1, 0, 0), (0, 1, 0), (0, 0, 1)
    # Each face: a corner, then two edges whose cross product points out of the cube
    mesh.add_quad(transform, (-0.5, -0.5, 0.5), x, y, material)     # +Z
    mesh.add_quad(transform, (0.5, -0.5, -0.5), neg(x), y, material)  # -Z
    mesh.add_quad(transform, (0.5, -0.5, 0.5), neg(z), y, material)   # +X
    mesh.add_quad(transform, (-0.5, -0.5, -0.5), z, y, material)    # -X
    mesh.add_quad(transform, (-0.5, 0.5, 0.5), x, neg(z), material)   # +Y
    mesh.add_quad(transform, (-0.5, -0.5, -0.5), x, z, material)    # -Y
    return mesh


# A UV sphere
def sphere(centre, radius, material, segments=48, rings=24):
    mesh = Mesh()
    for ring in range(rings + 1):
        polar = math.pi * ring / rings
        for segment in range(segments + 1):
            azimuth = 2.0 * math.pi * segment / segments
            normal = (math.sin(polar) * math.cos(azimuth), math.cos(polar), -math.sin(polar) * math.sin(azimuth))
            mesh.add_vertex(IDENTITY, add(centre, scale(normal, radius)), normal, material)
    # Rings run from the top (+Y) down, and segments counter-clockwise seen from above
    stride = segments + 1
    for ring in range(rings):
        for segment in range(segments):
            top = ring * stride + segment
            bottom = top + stride
            mesh.indices += [top, bottom, bottom + 1, bottom + 1, top + 1, top]
    return mesh


def box(centre, size, yaw, material):
    transform = mat_mul(mat_mul(translate(centre), rotate_y(yaw)), scaling(size))
    return cube(material, transform)


# Albedos are linear (sRGB colours would be decoded first); the last number is the roughness
SCENE = [
    ("ground", plane(40.0, (0.4, 0.4, 0.38, 0.9))),
    ("red box", box((-2.2, 0.5, 0.2), (1.0, 1.0, 1.0), 20.0, (0.75, 0.08, 0.06, 0.5))),
    ("blue box", box((2.2, 0.75, -1.2), (1.5, 1.5, 1.5), -35.0, (0.08, 0.2, 0.7, 0.35))),
    ("gold pillar", box((-0.4, 1.25, -3.0), (0.7, 2.5, 0.7), 10.0, (0.8, 0.6, 0.1, 0.7))),
    ("white sphere", sphere((0.2, 0.8, 1.2), 0.8, (0.9, 0.9, 0.9, 0.15))),
    ("green sphere", sphere((-2.6, 0.55, 2.4), 0.55, (0.1, 0.6, 0.15, 0.85))),
    ("orange sphere", sphere((2.8, 0.5, 1.8), 0.5, (0.9, 0.35, 0.05, 0.3))),
]


def main():
    data = bytearray()
    views, accessors, meshes, nodes = [], [], [], []

    def view(blob, target):
        while len(data) % 4:
            data.append(0)
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob), "target": target})
        data.extend(blob)
        return len(views) - 1

    def accessor(view_index, component, count, kind, bounds=None):
        a = {"bufferView": view_index, "componentType": component, "count": count, "type": kind}
        if bounds:
            a["min"], a["max"] = bounds
        accessors.append(a)
        return len(accessors) - 1

    for name, mesh in SCENE:
        count = len(mesh.vertices)
        f32 = lambda values: struct.pack("<%df" % len(values), *values)
        positions = [c for v in mesh.vertices for c in v[0]]
        normals = [c for v in mesh.vertices for c in v[1]]
        colors = [c for v in mesh.vertices for c in v[2]]
        rounded = [struct.unpack("<f", struct.pack("<f", c))[0] for c in positions]
        bounds = ([min(rounded[i::3]) for i in range(3)], [max(rounded[i::3]) for i in range(3)])
        attributes = {
            "POSITION": accessor(view(f32(positions), 34962), 5126, count, "VEC3", bounds),
            "NORMAL": accessor(view(f32(normals), 34962), 5126, count, "VEC3"),
            "COLOR_0": accessor(view(f32(colors), 34962), 5126, count, "VEC4"),
        }
        indices = accessor(view(struct.pack("<%dH" % len(mesh.indices), *mesh.indices), 34963), 5123,
                           len(mesh.indices), "SCALAR")
        meshes.append({"name": name, "primitives": [{"attributes": attributes, "indices": indices}]})
        nodes.append({"name": name, "mesh": len(meshes) - 1})

    gltf = {
        "asset": {"version": "2.0", "generator": "examples/scene/make_scene.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "buffers": [{"byteLength": len(data), "uri": "scene.bin"}],
        "bufferViews": views,
        "accessors": accessors,
    }
    directory = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "scene.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    with open(os.path.join(directory, "scene.bin"), "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
