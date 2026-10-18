#lang racket/base
;; ZGAL's errors: every error the compiler reports has an id from the catalog below, which the
;; documentation renders (with each example's actual message) and the tests check (each example
;; must report its id). Raising an id that isn't in the catalog is an internal error, so the
;; catalog can't fall behind the compiler.

(require racket/list
         racket/string)

(provide (struct-out loc)
         (struct-out exn:zgal)
         (struct-out error-kind)
         stx->loc
         zgal-error
         error-catalog
         error-kind-ref
         format-zgal-error)

;; A position in a source file (a .zgal file or a shader body). Lines and columns count from 1.
(struct loc (source line column) #:transparent)

(define (stx->loc stx)
  (loc (let ([source (syntax-source stx)])
         (if (path? source) (path->string source) (format "~a" source)))
       (or (syntax-line stx) 1)
       (add1 (or (syntax-column stx) 0))))

;; An error in the program being compiled: `where` is a loc or #f
(struct exn:zgal exn:fail (id where) #:transparent)

;; id: a symbol. title: a short description. explanation: what causes it and how to fix it.
;; example: a program reporting it. bodies: shader bodies the example uses (path -> text).
(struct error-kind (id title explanation example bodies) #:transparent)

(define (kind id title explanation example [bodies (hash)])
  (error-kind id title explanation example bodies))

(define error-catalog
  (list
   (kind 'read-error
         "The file isn't valid s-expressions"
         "Parentheses don't balance, or a token can't be read (e.g. a string isn't closed). The location is where the reader gave up, which for a missing `)` is often the end of the file: look for the form that doesn't close."
         "(struct Light\n  (direction vec3)")
   (kind 'unknown-form
         "Unknown top-level form"
         "A file is a sequence of `struct`, `vertex-format`, `define`, `graphics-material`, `compute-material`, `graph` and `app` forms, and a graph of the items and nodes the reference lists."
         "(structure Light (direction vec3))")
   (kind 'malformed
         "A form doesn't have the expected shape"
         "The message says what was expected, e.g. a struct field is `(name type)`. The reference gives every form's grammar."
         "(struct Light direction vec3)")
   (kind 'bad-identifier
         "Not a valid name"
         "Names become GLSL and C++ identifiers, so they are letters, digits and underscores, start with a letter and don't contain `__`. Lisp-style names such as `shadow-map` aren't translated: write `shadowMap`, the name the shaders and C++ use."
         "(struct Light (light-direction vec3))")
   (kind 'reserved-name
         "The name is reserved"
         "The name is a GLSL or C++ keyword, a GLSL built-in function (binding accessors are function-like macros, so a binding named `texture` would replace GLSL's `texture()`), a type, a name the wrapper's C++ uses (e.g. `Material`), or one the generated code uses (`main`, `materialPush`, anything starting with `zgal` or `gl_`). Functions and structs also can't be named after a form of bodies (`count`, `sum`, `with`, `at`...), as a call to one would mean the form; values (fields, locals, constants) can be."
         "(struct Light (sampler uint))")
   (kind 'duplicate-name
         "A name is declared twice"
         "Top-level names (structs, vertex formats, materials) are one namespace, as are the fields of a struct. Within a material, bindings, push constants, varyings, outputs and vertex inputs (`in` followed by the attribute's name) share the shaders' global scope, and can't reuse a struct's name either."
         "(struct Light (direction vec3) (direction vec3))")
   (kind 'unknown-type
         "Unknown type"
         "Types are `float`, `int`, `uint`, `bool`, the 8- and 16-bit integers `uint8`, `int8`, `uint16` and `int16`, the vectors (`vec2`-`vec4`, `ivec2`-`ivec4`, `uvec2`-`uvec4`), `mat2`, `mat3`, `mat4`, `(array type length)` and structs declared earlier in the file. A struct must be declared before it's used."
         "(struct Light (direction float3))")
   (kind 'unknown-name
         "A reference to something that isn't declared"
         "Names refer to declarations earlier in the file: a `vertex-input` to a `vertex-format` (and its attributes), a graph's material to a material, and a graph's items to the graph's parameters, inputs, resources and materials declared before them."
         "(vertex-format Vertex (position vec3))\n(graphics-material Flat\n  (shaders \"flat.vert\" \"flat.frag\")\n  (vertex-input Vertex normal)\n  (outputs (color vec4)))")
   (kind 'type-not-allowed
         "The type can't be used here"
         "Buffers and push constants use the std430 layout, which C++ must match byte for byte: `bool` has no fixed size there (use `uint`), and `mat3` and arrays of 3-component vectors are padded differently by GLM (use `mat4` and 4-component vectors). 8- and 16-bit integers aren't push constants (use `uint` or `int`). Vertex attributes, varyings and outputs are scalars or vectors of `float`, `int` or `uint`; an attribute's `#:as` storage converts to its kind (`float16`, `unorm8`, `snorm16`, ... for `float`, `uint8`/`uint16` for `uint`, `int8`/`int16` for `int`). Buffer bindings hold a struct, scalar, vector or matrix, not an array (use `buffer-array`)."
         "(struct Light (enabled bool))")
   (kind 'bad-array-length
         "An array's length isn't a positive integer"
         "Arrays in structs and push constants have a fixed length: `(array float 8)`. For arrays sized at runtime, use a `buffer-array` binding."
         "(struct Weights (values (array float 0)))")
   (kind 'unknown-clause
         "Unknown or misplaced material clause"
         "Graphics materials take `shaders`, `vertex`, `fragment`, `vertex-input`, `bindings`, `push`, `varyings` and `outputs`; compute materials take `shaders`, `compute`, `workgroup`, `bindings`, `push` and `shared`."
         "(compute-material Blur\n  (shaders \"blur.comp\")\n  (workgroup 16 16)\n  (outputs (color vec4)))")
   (kind 'missing-clause
         "A required material clause is missing"
         "Graphics materials need `outputs` (write `(outputs)` for depth-only materials): it fixes how many colour attachments their targets have. Compute materials need `workgroup`."
         "(graphics-material Flat\n  (shaders \"flat.vert\" \"flat.frag\"))")
   (kind 'duplicate-clause
         "A material clause appears twice"
         "Each clause appears at most once; list everything in one clause."
         "(graphics-material Flat\n  (shaders \"flat.vert\" \"flat.frag\")\n  (outputs (color vec4))\n  (outputs (normal vec4)))")
   (kind 'bad-shader
         "The material's shaders don't fit it"
         "A graphics material has one vertex and one fragment shader, and a compute material one compute shader, each either a ZGAL body (a `vertex`, `fragment` or `compute` clause) or a GLSL body in `shaders`: paths relative to the .zgal file, whose extension is their stage (`.vert`, `.frag` or `.comp`)."
         "(graphics-material Flat\n  (shaders \"flat.vert\")\n  (outputs (color vec4)))")
   (kind 'shader-not-found
         "A shader body or GLSL file doesn't exist"
         "Paths of GLSL bodies (in `shaders`) and of files of GLSL functions (`extern-glsl`) are relative to the .zgal file declaring them. They're only read when generating GLSL, so checking a file (`--check`) doesn't report this."
         "(graphics-material Flat\n  (shaders \"missing.vert\" \"missing.frag\")\n  (outputs (color vec4)))")
   (kind 'shader-reused
         "Two materials use the same shader"
         "Each shader body is compiled with its material's interface, so it belongs to one material. The shader library finds shaders by name (a GLSL body's file name; a ZGAL body's material name and stage, e.g. `Lit.frag`), so two shaders can't share one either, even in different directories. Share code between shaders with functions (`define`), or with GLSL `#include` in GLSL bodies."
         "(graphics-material A\n  (shaders \"flat.vert\" \"flat.frag\")\n  (outputs (color vec4)))\n(graphics-material B\n  (shaders \"flat.vert\" \"other.frag\")\n  (outputs (color vec4)))")
   (kind 'bad-binding
         "A binding's kind or access is invalid"
         "A binding is `(name kind)`, where the kind is `texture`, `sampler`, `(storage-image [access])`, `(buffer type [access])`, `(buffer-array type [access])` or `(array kind)` (an array of any of those but samplers), and the access is `read`, `write` or `read-write`. The access is what the render graph synchronizes on. It's inferred when every shader of the material is a ZGAL body, and must be declared, accurately, when one is GLSL."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 64)\n  (bindings (result (buffer float))))")
   (kind 'bad-stages
         "A binding's stages are invalid"
         "`#:stages` lists the shader stages using a graphics material's binding (`vertex`, `fragment`), so the render graph synchronizes with the right ones; without it, both. Compute materials' bindings are always used by the compute stage, and samplers aren't synchronized, so neither takes `#:stages`."
         "(graphics-material Flat\n  (shaders \"flat.vert\" \"flat.frag\")\n  (bindings (albedo texture #:stages (geometry)))\n  (outputs (color vec4)))")
   (kind 'push-too-large
         "The push constants don't fit"
         "A material's push constants are a 32-bit index per binding followed by its `push` fields, and the wrapper's pipeline layout has 128 bytes of them. Move large or rarely changing data into a buffer."
         "(compute-material Big\n  (shaders \"big.comp\")\n  (workgroup 64)\n  (push (a mat4) (b mat4) (c float)))")
   (kind 'varying-not-flat
         "An integer varying isn't flat"
         "Vulkan doesn't interpolate integers: declare integer varyings `flat`, e.g. `(id uint flat)`."
         "(graphics-material Ids\n  (shaders \"ids.vert\" \"ids.frag\")\n  (varyings (id uint))\n  (outputs (color vec4)))")
   (kind 'bad-workgroup
         "The workgroup size is invalid"
         "`(workgroup x [y [z]])` gives one to three positive integers, the compute shader's local size; omitted dimensions are 1."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 0))")
   (kind 'type-mismatch
         "A value has the wrong type, or a name the wrong kind"
         "Graph expressions are typed: `if` needs a bool condition and branches of one type, arithmetic operands share a type (integer literals adapt to it), and a push constant's value has the field's type. A name used as a value is a parameter, constant or per-frame input, and one used as a resource is of the kind the place needs (e.g. a target, a material)."
         "(graph G\n  (parameter size float)\n  (constant halfSize (/ size 2))\n  (image small rgba8 (fixed halfSize halfSize) #:frames shared))")
   (kind 'not-build-time
         "A per-frame input where a value is computed once"
         "Conditions (`#:when`, `#:enabled`), constants, extents and vertex counts are computed when the graph is built, from parameters, constants and imported images' sizes. Per-frame inputs, and the sizes of the graph's own images (which it can recreate, e.g. when the surface resizes), are only known each frame, so only push constants, compute materials' `#:invocations` and buffers' counts can use them."
         "(graph G\n  (input size (per-frame uint))\n  (image i rgba8 (fixed size size) #:frames shared))")
   (kind 'binding-mismatch
         "A material binding is given the wrong kind of resource"
         "Textures and storage images take images, samplers take samplers, `buffer` bindings take a uniform or a buffer (or imported buffer) of their type, and `buffer-array` bindings take a buffer (or imported buffer) of their element type. Array bindings take a list of such resources, e.g. `(sources (a b))`, or (for arrays of images) imported images. Uniforms are written by the CPU, so a binding writing one is an error."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 64)\n  (bindings (target (storage-image write))))\n(graph G\n  (sampler s)\n  (material fill Fill (target s) #:invocations (64))\n  (dispatch fill fill))")
   (kind 'incomplete-material
         "A material in a graph doesn't give a binding or push constant"
         "A graph's material gives a resource for every binding of the material it instantiates, and a value for every push constant (`#:push ((field value) ...)`), unless every node using it gives that push constant itself (`#:push` on dispatches and draws)."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 64)\n  (push (value float)))\n(graph G\n  (material fill Fill #:invocations (64))\n  (dispatch fill fill))")
   (kind 'loop-body
         "A loop holds what it can't"
         "A graph's `for` loop repeats nodes (dispatch, draw, draw-meshes, readback, cpu, copy, clear) and loops: declare materials, buffers and other items before it, and give each iteration's values with nodes' options (e.g. `#:push`, `#:invocations`). Extern nodes, the compute library's nodes and draws with a `#:count` aren't in loops yet."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 64))\n(graph G\n  (for ([i (range 0 4)])\n    (material fill Fill #:invocations (64))\n    (dispatch fill fill)))")
   (kind 'material-kind
         "A node is given a material of the wrong kind"
         "`dispatch` runs compute materials. `draw-meshes` draws graphics materials with a `vertex-input` (from mesh vertex buffers), and `draw` graphics materials without one (a vertex count, e.g. a fullscreen triangle)."
         "(graphics-material Flat\n  (shaders \"flat.vert\" \"flat.frag\")\n  (outputs (color vec4)))\n(graph G\n  (target t (surface #:depth none))\n  (material quad Flat)\n  (dispatch run quad))")
   (kind 'material-targets
         "A graphics material is drawn into more than one target"
         "A graphics material's pipeline is built for its target's formats, so each target needs a material of its own: declare one per target."
         "(graphics-material Flat\n  (shaders \"flat.vert\" \"flat.frag\")\n  (outputs (color vec4)))\n(graph G\n  (image a rgba8 (fixed 4 4) #:frames shared)\n  (image b rgba8 (fixed 4 4) #:frames shared)\n  (target ta (color a))\n  (target tb (color b))\n  (material quad Flat)\n  (draw first ta quad 3)\n  (draw second tb quad 3))")
   (kind 'unused-material
         "No node uses a graph's material"
         "A graph's materials are built for the nodes using them (a graphics material for its target's formats), so a material no node uses is a mistake: remove it, or add the node."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 64))\n(graph G\n  (material fill Fill #:invocations (64)))")
   (kind 'unknown-format
         "Unknown image format"
         "Image formats are written like `rgba8`, `rgba8-srgb`, `rgba16f` or `d32`; the reference lists them all with their Vulkan formats."
         "(graph G\n  (image i rgba10 (fixed 4 4) #:frames shared))")
   (kind 'bad-option
         "A keyword option is missing, unknown or invalid"
         "Options follow a form's positional arguments as `#:keyword value`. The message says what the form takes; some are required, e.g. an image's `#:frames` (`shared` or `per-frame`, a trade-off the wrapper makes explicit too)."
         "(graph G\n  (image i rgba8 (fixed 4 4)))")
   (kind 'model-attribute
         "A vertex format's attribute a model can't fill"
         "A graph's `model` fills its vertex format's attributes by name: `position` (or `pos`) a `vec2`, `vec3` or `vec4` (w 1), `normal` a `vec3` or `vec4` (w 0), `uv` (or `uv0`, `texCoord`) a `vec2`, `color` (or `colour`) and `tangent` a `vec3` or `vec4`, converted to the attribute's `#:as` storage. Other attributes need a `#:default` value, which also fills the named ones when a file lacks them."
         "(vertex-format Vertex (position vec3) (shininess float))\n(graph G\n  (model ship \"ship.gltf\" Vertex))")
   (kind 'access-mismatch
         "A declared access or stage contradicts the shaders"
         "When all of a material's shaders are ZGAL bodies, its bindings' accesses and stages are inferred from what the bodies do. A declared one must say the same: remove it, or fix the body."
         "(compute-material Fill\n  (workgroup 64)\n  (bindings (result (buffer-array float read)))\n  (compute\n   (write! (at result globalInvocationId.x) 1.0)))")
   (kind 'unused-binding
         "A binding's access can't be inferred, as no shader uses it"
         "A storage binding's access is inferred from the shaders using it. If none does, remove the binding, or declare its access."
         "(compute-material Fill\n  (workgroup 64)\n  (bindings (unused (buffer float)))\n  (compute))")
   (kind 'bad-write
         "write! writes to something that isn't a buffer"
         "`write!` writes a buffer binding's value or a shared variable, or a field or element of one, e.g. `(write! (at particles i) p)`, and atomics (`atomic-add!` and the others) work on such a place's element or field. Locals are immutable: bind a new one with `let*` instead. Images are written with `store!`."
         "(compute-material Fill\n  (workgroup 64)\n  (bindings (result (buffer-array float)))\n  (compute\n   (let* ([x 1.0])\n     (write! x 2.0))))")
   (kind 'bad-results
         "A shader's results are missing, unknown or in the wrong place"
         "A vertex shader's body gives `(results #:position ... #:varying ...)`: its position and every varying. A fragment shader's gives `(results #:output ...)` with every output, or its one output's value. Compute shaders have no results."
         "(graphics-material Flat\n  (varyings (uv vec2))\n  (outputs (color vec4))\n  (vertex\n   (results #:position (vec4 0.0)))\n  (fragment\n   (vec4 1.0)))")
   (kind 'wrong-stage
         "A form or built-in used in a stage it isn't for"
         "`(return!)` ends a compute shader's invocation and `(discard!)` a fragment shader's, so each is only in that stage's body (not functions, which return values). Derivatives (`dFdx`, `dFdy`, `fwidth` and their variants) compare neighbouring fragment shader invocations, so only fragment shaders (and functions they call) use them."
         "(compute-material Fill\n  (workgroup 64)\n  (bindings (result (buffer-array float)))\n  (compute\n   (when (> globalInvocationId.x 100)\n     (discard!))\n   (write! (at result globalInvocationId.x) 1.0)))")
   (kind 'bad-barrier
         "A barrier! some invocations might not reach"
         "`barrier!` waits for every invocation of the workgroup to reach it, so it's only in compute shaders, and every invocation must reach it or none: it can't be under a condition, or in a loop, that could differ between invocations. Conditions and ranges computed from literals, constants, push constants, `workgroupId` and `workgroupCount` are the same for the whole workgroup; move the barrier out of anything else, e.g. `(when (< x stride) (write! ...))` followed by `(barrier!)`."
         "(compute-material Reduce\n  (workgroup 64)\n  (shared (partial (array float 64)))\n  (bindings (result (buffer float)))\n  (compute\n   (when (< localInvocationId.x 32)\n     (barrier!))\n   (write! result (at partial 0))))")
   (kind 'bad-copy
         "A copy node's buffers don't fit"
         "A `copy` node copies elements between two different buffers (graph or imported) holding the same element type; its `#:count` and offsets count elements."
         "(graph G\n  (buffer a float 16 #:frames shared)\n  (buffer b uint 16 #:frames shared)\n  (copy c a b))")
   (kind 'gpu-only
         "An app calls a function only shaders can run"
         "App bodies run on the CPU, so they call functions that compile to C++. Functions written in GLSL (`extern-glsl`), and ZGAL functions calling them, only compile for shaders."
         "(extern-glsl \"noise.glsl\" ((noise [p : vec2]) : float))\n(app A\n  (state (x float 0.0))\n  (on (key-pressed space) (with state #:x (noise (vec2 1.0)))))")
   (kind 'bad-import
         "An import can't be found or used"
         "`(import name)` imports a module of the standard library (`dsl/lib`, listed in the reference), and `(import \"file.zgal\")` a file, relative to the importing one. Imported files hold only structs, vertex formats, functions, constants, materials and imports, which become declarations of the importing file. A file can't import itself, even indirectly."
         "(import geometry)")
   (kind 'bad-effect
         "An app's effect outside an app"
         "`print!`, `set-frames-in-flight!`, `resize!` and `quit!` act on the running program, so only an app's handlers use them; shaders and functions compute values (and shaders write buffers and images)."
         "(define (shout [x : float]) : float\n  (begin (print! x) x))")
   (kind 'version-in-body
         "A shader body declares what ZGAL generates"
         "ZGAL writes the start of every shader: `#version`, the extensions it needs and `bindless.glsl`, then the material's interface. Bodies start with their own code, so remove the line."
         "(compute-material Fill\n  (shaders \"fill.comp\")\n  (workgroup 64))"
         (hash "fill.comp" "#version 450\nvoid main() {}\n"))))

(define catalog-by-id
  (for/hash ([k (in-list error-catalog)]) (values (error-kind-id k) k)))

(define (error-kind-ref id) (hash-ref catalog-by-id id #f))

;; Raises an error in the program being compiled. where: a syntax object, a loc or #f.
(define (zgal-error id where fmt . args)
  (unless (error-kind-ref id)
    (error 'zgal "internal error: error id ~a isn't in the catalog" id))
  (define location (cond [(syntax? where) (stx->loc where)]
                         [(loc? where) where]
                         [else #f]))
  (raise (exn:zgal (apply format fmt args) (current-continuation-marks) id location)))

;; "file:line:column: error: message [id]", the format compilers use, so editors can jump to it
(define (format-zgal-error e)
  (define where (exn:zgal-where e))
  (string-append
   (if where (format "~a:~a:~a: " (loc-source where) (loc-line where) (loc-column where)) "")
   (format "error: ~a [~a]" (exn-message e) (exn:zgal-id e))))
