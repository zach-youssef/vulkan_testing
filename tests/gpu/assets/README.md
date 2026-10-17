# Test assets

Small files the GPU and CPU tests load (the tests' asset search path):

- `quadrants.png`: 2x2 RGBA8, red, green (top), blue, grey 128 (bottom).
- `halves.gltf`: two unit quads sharing a buffer (a data URI), placed by their nodes on the left and
  right halves of clip space: the left one textured with `quadrants.png`, the right one green (base
  colour factor (0, 0.5, 0, 1)).
- `quad.obj` and `quad.mtl`: a triangle without a material, then a quad with material `tinted`
  (diffuse (0.5, 0.25, 1), textured with `quadrants.png`).

- `broken.gltf`: truncated JSON, which fails to parse.

They were written by hand (the glTF's buffer by a short script packing the floats and indices).
