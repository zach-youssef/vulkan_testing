#lang racket/base
;; What compiling for the web reports that Vulkan allows (not-on-web): what WebGPU can't do, and what
;; the web backend doesn't do yet; how it compiles what some WebGPU implementations refuse; and the
;; web interface of GLSL bodies' shaders

(require racket/list
         racket/string
         rackunit
         "../compile.rkt"
         "../private/ast.rkt"
         "../private/errors.rkt"
         "../private/web-glsl.rkt"
         "util.rkt")

;; Checks compiling text for the web reports not-on-web, with a message containing fragment
(define (check-not-on-web text fragment)
  (define e (zgal-error-of text #:web? #t))
  (with-check-info (['program text])
    (check-true (exn:zgal? e))
    (when (exn:zgal? e)
      (check-equal? (exn:zgal-id e) 'not-on-web)
      (check-true (string-contains? (exn-message e) fragment) (exn-message e)))))

(define corner
  "(graphics-material Corner
     (outputs (color vec4))
     (vertex (results #:position (vec4 0.0 0.0 0.0 1.0)))
     (fragment (vec4 1.0)))\n")

(define layered
  "(compute-material Layered
     (workgroup 1)
     (bindings (layers (array texture)) (counts (buffer-array uint)))
     (compute (write! (at counts 0) (uint (.x (texture-size (at layers 1) 0))))))\n")

(test-case "Materials WebGPU can't express"
  (check-not-on-web
   "(compute-material Sum
      (workgroup 1)
      (bindings (sources (array (buffer-array uint))) (total (buffer-array uint)))
      (compute (write! (at total 0) (at (at sources 0) 0))))
    (graph Sums
      (input shown (per-frame (data uint)))
      (buffer a uint 1 #:frames shared)
      (buffer total uint 1 #:frames shared)
      (material summing Sum (sources (a) #:select shown) total #:invocations (1))
      (dispatch run summing))"
   "can't #:select its elements"))


(test-case "Graphs binding arrays of textures: copied image files, and graph and imported images"
  (define (graph . items)
    (string-append layered "(graph G\n  (buffer counts uint 1 #:frames shared)\n  " (string-join items "\n  ")
                   "\n  (dispatch d m #:invocations (1)))"))
  (define (web-ts text)
    (define-values (_prog outputs)
      (compile-zgal "test.zgal" #:text text #:read-body (λ (path) #f) #:native? #f #:web-dir "out/web"))
    (output-text (findf (λ (o) (string-suffix? (output-path o) ".zgal.ts")) outputs)))
  ;; Image files of either colour space, as their elements' buckets are per format
  (define files (web-ts (graph "(image-file a \"a.png\" #:color srgb)" "(image-file b \"b.png\" #:color linear)"
                               "(material m Layered (layers (a b a)) counts)")))
  (check-true (string-contains? files "const array_m_layers = graph.textureArray('m\\'s layers', [image_a, image_b, image_a]);") files)
  (check-true (string-contains? files "{ layers: { array: array_m_layers }, counts: { buffer: buffer_counts } }") files)
  ;; Graph images (one only some frames), mixed with an image file: buckets of their own
  (define images (web-ts (graph "(parameter withB bool)" "(image a rgba8 (fixed 4 4) #:frames shared)"
                                "(image b rgba8 (surface 0.5) #:frames shared #:when withB)" "(image-file c \"c.png\" #:color linear)"
                                "(material m Layered (layers (a c b)) counts)")))
  (check-true (string-contains? images "const array_m_layers = graph.textureArray('m\\'s layers', [image_a, image_c, need(image_b, 'b')]);") images)
  ;; Imported images, which the application gives
  (define imported (web-ts (graph "(import-images given #:resting shader-read-only)" "(material m Layered (layers given) counts)")))
  (check-true (string-contains? imported "  given: readonly GPUTexture[];") imported)
  (check-true (string-contains? imported "const images_given = graph.importImages('given', inputs.given, 'live');") imported)
  (check-true (string-contains? imported "result.images.given = images_given;") imported)
  (check-true (string-contains? imported "const array_m_layers = graph.textureArray('m\\'s layers', images_given);") imported))

;; The WGSL compiling text for the web gives for shader `name` (e.g. "Stamp.comp.wgsl")
(define (web-shader text name)
  (define-values (_prog outputs)
    (compile-zgal "test.zgal" #:text text #:read-body (λ (path) #f) #:native? #f #:web-dir "out/web"))
  (output-text (findf (λ (o) (string-suffix? (output-path o) name)) outputs)))

(test-case "Struct and array push constants are read from a uniform-layout mirror, converted at the start of main"
  (define program
    "(struct Inner (w float) (k uint))
     (struct Packed (a vec2) (vals (array float 3)) (inner Inner) (tail float) (m mat2) (cols (array vec2 2)) (inners (array Inner 2)))
     (graphics-material Show
       (push (lead float) (p Packed) (after uint) (weights (array vec4 2)))
       (define (pick [i : uint]) : float (at (.vals p) i))
       (varyings (shade float))
       (outputs (color vec4))
       (vertex (results #:position (vec4 lead 0.0 0.0 1.0) #:shade (pick 1)))
       (fragment (vec4 (.w (.inner p)) shade (float after) (.x (at weights 1)))))\n")
  (define vertex (web-shader program "Show.vert.wgsl"))
  (define fragment (web-shader program "Show.frag.wgsl"))
  (define ts (web-shader program "test.zgal.ts"))
  (define (has text line) (check-true (string-contains? text (string-append "\n" line "\n")) line))
  (with-check-info (['wgsl vertex])
    ;; The std430 shape the body reads, and its mirror: arrays of scalars and 2-component vectors
    ;; are of 4-component ones, arrays and structs (and a mirror's first member) aligned to 16
    (for ([l (list "struct ZgalPush {" "    lead: f32," "    p: Packed," "    after: u32," "    weights: array<vec4f, 2>,"
                   "struct ZgalUniform_Inner {" "    @align(16) w: f32," "    k: u32,"
                   "struct ZgalUniform_Packed {" "    @align(16) a: vec2f," "    @align(16) vals: array<vec4f, 3>,"
                   "    @align(16) inner: ZgalUniform_Inner," "    tail: f32," "    m: mat2x2f,"
                   "    @align(16) cols: array<vec4f, 2>," "    @align(16) inners: array<ZgalUniform_Inner, 2>,"
                   "struct ZgalPushUniform {" "    @align(16) p: ZgalUniform_Packed," "    @align(16) weights: array<vec4f, 2>,"
                   "@group(0) @binding(0) var<uniform> zgalPushUniform: ZgalPushUniform;"
                   "fn zgal_push() -> ZgalPush {"
                   "    for (var zgalI0 = 0u; zgalI0 < 3u; zgalI0++) {"
                   "        zgalValue.p.vals[zgalI0] = zgalPushUniform.p.vals[zgalI0].x;"
                   "        zgalValue.p.cols[zgalI0] = zgalPushUniform.p.cols[zgalI0].xy;"
                   "        zgalValue.p.inners[zgalI0].k = zgalPushUniform.p.inners[zgalI0].k;"
                   "    zgalValue.p.m = zgalPushUniform.p.m;"
                   ;; Arrays whose elements are 16 bytes apart in both are copied whole
                   "    zgalValue.weights = zgalPushUniform.weights;")])
      (has vertex l))
    (check-false (string-contains? vertex "var<uniform> zgalPush:"))
    ;; Converted once at the start of main (which material functions are inlined into), into a
    ;; local, so that values read from it stay uniform for WGSL's uniformity analysis
    (check-equal? (length (regexp-match* #rx"\n    var zgalPush = zgal_push[(][)];\n" vertex)) 1))
  (with-check-info (['wgsl fragment])
    (check-equal? (length (regexp-match* #rx"\n    var zgalPush = zgal_push[(][)];\n" fragment)) 1))
  (with-check-info (['ts ts])
    ;; The runtime writes the values at the mirror's offsets
    (has ts "  push: { binding: 0, size: 240, fields: {")
    (has ts "    lead: { offset: 0, type: zgal.float },")
    (has ts (string-append "    p: { offset: 16, type: zgal.uniformStruct<Packed>('Packed', [['a', zgal.vec2], ['vals', zgal.uniformArray(zgal.float, 3)], "
                           "['inner', zgal.uniformStruct<Inner>('Inner', [['w', zgal.float], ['k', zgal.uint]])], ['tail', zgal.float], ['m', zgal.mat2], "
                           "['cols', zgal.uniformArray(zgal.vec2, 2)], ['inners', zgal.uniformArray(zgal.uniformStruct<Inner>('Inner', [['w', zgal.float], ['k', zgal.uint]]), 2)]]) },"))
    (has ts "    after: { offset: 192, type: zgal.uint },")
    (has ts "    weights: { offset: 208, type: zgal.uniformArray(zgal.vec4, 2) },")))

(test-case "Push constants of scalars, vectors and matrices are read from the uniform buffer directly"
  (define wgsl
    (web-shader "(compute-material C (workgroup 1) (bindings (sums (buffer-array float))) (push (scale float) (offset vec2))
                   (compute (write! (at sums 0) (+ scale (.y offset)))))\n"
                "C.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-true (string-contains? wgsl "\n@group(0) @binding(1) var<uniform> zgalPush: ZgalPush;\n"))
    (check-false (string-contains? wgsl "zgal_push"))
    (check-false (string-contains? wgsl "ZgalPushUniform"))))

(test-case "Functions with storage image parameters are inlined: Firefox's naga refuses them as WGSL parameters"
  (define wgsl
    (web-shader
     "(define (paint [image : image2D] [at : ivec2] [shade : float]) : void
        (store! image at (vec4 shade)))
      ;; Passes its image on
      (define (paintPair [image : image2D] [at : ivec2]) : void
        (paint image at 0.5)
        (paint image (+ at (ivec2 1 0)) 1.0))
      (compute-material Stamp
        (workgroup 1)
        (bindings (first (storage-image write)) (second (storage-image write)))
        (compute
          (paint first (ivec2 0 0) 0.25)
          (paint second (ivec2 0 0) 0.75)
          (paintPair second (ivec2 0 1))))"
     "Stamp.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-false (string-contains? wgsl "fn paint"))
    (check-false (regexp-match? #rx"[(,] *[a-zA-Z_0-9]+: texture_storage" wgsl))
    (check-equal? (length (regexp-match* #rx"textureStore\\(first, " wgsl)) 1)
    (check-equal? (length (regexp-match* #rx"textureStore\\(second, " wgsl)) 3)))

(test-case "Inlined functions' texture and sampler arguments are substituted too: WGSL has no locals of them"
  (define wgsl
    (web-shader
     "(define (copy [image : image2D] [source : texture2D] [s : sampler]) : void
        (store! image (ivec2 0 0) (sample-lod source s (vec2 0.5) 0.0)))
      (compute-material Copy
        (workgroup 1)
        (bindings (img (storage-image write)) (tex texture) (smp sampler))
        (compute (copy img tex smp)))"
     "Copy.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-false (regexp-match? #rx"\n +var [a-zA-Z_0-9]+: (texture|sampler)" wgsl) "no locals of them")
    (check-true (string-contains? wgsl "textureStore(img, vec2i(0i, 0i), zgalSampleLevel(tex, smp, "))))

(test-case "An inlined function's texture array element argument has its index evaluated once, in order, on the web"
  (define wgsl
    (web-shader
     "(define (f [x : uint] [i : image2D] [t : texture2D] [s : sampler] [y : uint]) : void
        (store! i (ivec2 (int x) (int y)) (+ (sample-lod t s (vec2 0.5) 0.0) (sample-lod t s (vec2 0.1) 0.0))))
      (compute-material C
        (workgroup 1)
        (bindings (img (storage-image write)) (layers (array texture)) (smp sampler) (counts (buffer-array uint)))
        (compute (f (atomic-add! (at counts 0) (uint 1)) img (at layers (atomic-add! (at counts 1) (uint 1))) smp
                    (atomic-add! (at counts 0) (uint 2)))))"
     "C.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-equal? (length (regexp-match* #rx"atomicAdd\\(&counts\\[\\(4u\\)" wgsl)) 1)
    (check-regexp-match #rx"atomicAdd\\(&counts\\[\\(0u\\).*atomicAdd\\(&counts\\[\\(4u\\).*atomicAdd\\(&counts\\[\\(0u\\)"
                        (regexp-replace* "\n" wgsl " "))))

;; ---------------------------------------------------------------------------------------------
;; GLSL bodies' shaders for the web (web-glsl.rkt): the GLSL backend's web interface, which glslc
;; and naga turn into a WGSL module bound like a ZGAL body's (toolchain-test.rkt runs the tools)

;; The web-flavoured GLSL of the first shader of the first material of text, with body as its body
(define (web-glsl text body)
  (define-values (prog _outputs) (compile-zgal "test.zgal" #:text text #:check-only? #t))
  (define m (first (program-materials prog)))
  (define s (first (material-shaders m)))
  (emit-web-shader prog m s body (string-append "out/web/" (shader-name s)) #:storage-format (λ (b) "rgba8")))

(test-case "A GLSL body's shader for the web declares the WGSL bind group: a descriptor per binding at its index, the push constants a uniform block after them"
  (define glsl
    (web-glsl
     "(struct Lights (count uint))
      (graphics-material S
        (shaders \"shaders/s.vert\" \"shaders/s.frag\")
        (bindings (lights (buffer Lights read)) (sums (buffer-array float read-write)) (albedo texture)
                  (s sampler) (target (storage-image write)) (mask (storage-image read)))
        (push (tint vec3) (object uint))
        (varyings (uv vec2))
        (outputs (color vec4)))"
     "void main() {\n    gl_Position = vec4(0.0);\n}\n"))
  (define (has line) (check-true (string-contains? glsl (string-append "\n" line "\n")) line))
  (with-check-info (['glsl glsl])
    (has "#extension GL_EXT_scalar_block_layout : require")
    (has "layout(std430, set = 0, binding = 6) uniform ZgalPush {")
    (has "    vec3 tint;")
    (has "    uint object;")
    (has "} materialPush;")
    (has "layout(std430, set = 0, binding = 0) readonly buffer ZgalBlock_lights { Lights data; } zgal_lights;")
    (has "layout(std430, set = 0, binding = 1) buffer ZgalBlock_sums { float data[]; } zgal_sums;")
    (has "layout(set = 0, binding = 2) uniform texture2D zgal_albedo;")
    (has "layout(set = 0, binding = 3) uniform sampler zgal_s;")
    (has "layout(set = 0, binding = 4, rgba8) uniform writeonly image2D zgal_target;")
    (has "layout(set = 0, binding = 5, rgba8) uniform readonly image2D zgal_mask;")
    ;; Accessors as the bindless interface's, so bodies are the same
    (has "#define lights() (zgal_lights.data)")
    (has "#define sums() (zgal_sums.data)")
    (has "#define albedo() (zgal_albedo)")
    (has "#define s() (zgal_s)")
    (has "#define target() (zgal_target)")
    (has "#define tint() (materialPush.tint)")
    (has "layout(location = 0) out vec2 uv;")
    (has "#line 1 \"shaders/s.vert\"")
    (has "    gl_Position = vec4(0.0);")
    (check-false (string-contains? glsl "bindless"))
    (check-false (string-contains? glsl "push_constant"))))

(test-case "A GLSL body's shader for the web has no push constant block without push constants, and no index block"
  (define glsl (web-glsl "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (sums (buffer-array float read-write))))"
                         "void main() {}\n"))
  (with-check-info (['glsl glsl])
    (check-false (string-contains? glsl "ZgalPush"))
    (check-false (string-contains? glsl "zgalIndices"))
    (check-true (string-contains? glsl "\nlayout(std430, set = 0, binding = 0) buffer ZgalBlock_sums { float data[]; } zgal_sums;\n"))))

(test-case "A function's texture parameter isn't nonuniformEXT for the web, which has no arrays of bindings nor the extension"
  (define glsl
    (web-glsl
     "(define (texel [t : texture2D] [s : sampler] [k : int]) : vec4 (fetch t s (ivec2 k 0) 0))
      (compute-material C (workgroup 1)
        (bindings (picture texture) (smp sampler) (dest (buffer-array vec4)))
        (compute (write! (at dest 0) (texel picture smp 1))))"
     #f))
  (with-check-info (['glsl glsl])
    (check-true (string-contains? glsl "texelFetch(sampler2D(t, s), ivec2(k, 0), 0)"))
    (check-false (string-contains? glsl "nonuniformEXT"))))

(test-case "What a GLSL body's web interface can't express is refused before the tools run"
  (define (check-refused text fragment)
    (define e (with-handlers ([exn:zgal? values]) (web-glsl text "void main() {}\n") #f))
    (with-check-info (['program text])
      (check-true (exn:zgal? e) "expected an error")
      (when (exn:zgal? e)
        (check-equal? (exn:zgal-id e) 'not-on-web)
        (check-true (loc? (exn:zgal-where e)))
        (check-true (string-contains? (exn-message e) fragment) (exn-message e)))))
  (check-refused "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (decals (array texture))))"
                 "array of bindings")
  (check-refused "(struct P (a uint8))\n(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (ps (buffer-array P read))))"
                 "8- and 16-bit")
  (check-refused "(compute-material C (shaders \"c.comp\") (workgroup 1) (shared (marks (array uint8 4))))"
                 "8- and 16-bit")
  ;; The uniform block is std430: arrays and structs, which a uniform buffer lays out differently,
  ;; would be read at the wrong offsets (ZGAL bodies read a mirror instead)
  (check-refused "(struct P (a uint))\n(compute-material C (shaders \"c.comp\") (workgroup 1) (push (p P)))"
                 "push constant p is P")
  (check-refused "(compute-material C (shaders \"c.comp\") (workgroup 1) (push (n uint) (weights (array float 4))))"
                 "push constant weights is")
  (check-refused "(struct P (a uint))\n(compute-material C (shaders \"c.comp\") (workgroup 1) (push (ps (array P 2))))"
                 "push constant ps is"))

(test-case "A sampler sampling and comparing is bound as the variants WebGPU's samplers need, and functions are specialized for the depth textures they're given"
  (define wgsl
    (web-shader
     "(define (both [map : texture2D] [s : sampler] [uv : vec2]) : vec2
        (vec2 (.x (sample map s uv)) (sample-compare map s uv 0.5)))
      (define (red [t : texture2D] [s : sampler] [uv : vec2]) : float
        (.x (sample t s uv)))
      (compute-material Shade
        (workgroup 1)
        (bindings (shadow texture) (smp sampler) (picture texture) (results (buffer-array vec4)))
        (push (uv vec2))
        (compute
          (write! (at results 0)
                  (vec4 (both shadow smp uv) (red shadow smp uv) (red picture smp uv)))))"
     "Shade.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    ;; The variants, in the material's order, then the push constants
    (check-true (string-contains? wgsl "@group(0) @binding(0) var shadow: texture_depth_2d;"))
    (check-true (string-contains? wgsl "@group(0) @binding(1) var smp: sampler;"))
    (check-true (string-contains? wgsl "@group(0) @binding(2) var zgalDepth_smp: sampler;"))
    (check-true (string-contains? wgsl "@group(0) @binding(3) var zgalCompare_smp: sampler_comparison;"))
    (check-true (string-contains? wgsl "@group(0) @binding(4) var picture: texture_2d<f32>;"))
    (check-true (string-contains? wgsl "@group(0) @binding(6) var<uniform> zgalPush: ZgalPush;"))
    ;; What the sampler's variants can't say, for the WGSL to do by hand: whether it filters, its
    ;; border and, as it compares, its comparison
    (check-true (string-contains? wgsl "override zgalLinear_smp: bool;\noverride zgalBorder_smp: u32;\noverride zgalCompareOp_smp: u32;"))
    (check-true (string-contains? wgsl "fn zgalSampleDepth(t: texture_depth_2d, s: sampler, uv: vec2f, level: i32, linear: bool, border: u32) -> f32"))
    (check-true (string-contains? wgsl "fn zgalSampleCompareLevel(t: texture_depth_2d, s: sampler_comparison, uv: vec2f, reference: f32, linear: bool, border: u32, op: u32) -> f32"))
    (check-true (string-contains? wgsl "fn zgalSampleLevel(t: texture_2d<f32>, s: sampler, uv: vec2f, level: f32, linear: bool, border: u32) -> vec4f"))
    ;; A function's sampler is the variants it uses, then those properties; one only sampling is
    ;; specialized for a depth texture (its sampler then the non-filtering variant), the colour
    ;; texture's keeps its name
    (check-true (string-contains? wgsl "fn both(map: texture_depth_2d, zgalDepth_s: sampler, zgalCompare_s: sampler_comparison, zgalLinear_s: bool, zgalBorder_s: u32, zgalCompareOp_s: u32, uv: vec2f) -> vec2f"))
    (check-true (string-contains? wgsl "zgalSampleDepth(map, zgalDepth_s, uv, 0i, zgalLinear_s, zgalBorder_s)"))
    (check-true (string-contains? wgsl "zgalSampleCompareLevel(map, zgalCompare_s, uv, 0.5f, zgalLinear_s, zgalBorder_s, zgalCompareOp_s)"))
    (check-true (string-contains? wgsl "fn red(t: texture_2d<f32>, s: sampler, zgalLinear_s: bool, zgalBorder_s: u32, uv: vec2f) -> f32"))
    (check-true (string-contains? wgsl "zgalSampleLevel(t, s, uv, 0.0f, zgalLinear_s, zgalBorder_s).x"))
    (check-true (string-contains? wgsl "fn zgalDepth_0_red(t: texture_depth_2d, zgalDepth_s: sampler, zgalLinear_s: bool, zgalBorder_s: u32, uv: vec2f) -> f32"))
    (check-true (string-contains? wgsl "both(shadow, zgalDepth_smp, zgalCompare_smp, zgalLinear_smp, zgalBorder_smp, zgalCompareOp_smp, zgalPush.uv)"))
    (check-true (string-contains? wgsl "zgalDepth_0_red(shadow, zgalDepth_smp, zgalLinear_smp, zgalBorder_smp, zgalPush.uv)"))
    (check-true (string-contains? wgsl "red(picture, smp, zgalLinear_smp, zgalBorder_smp, zgalPush.uv)"))))

(test-case "Sampling past a border is by hand, from texels loaded, and only where the sampler has one"
  (define wgsl
    (web-shader
     "(graphics-material Edge
        (bindings (picture texture) (shadow texture) (smp sampler) (cmp sampler))
        (outputs (color vec4))
        (vertex (results #:position (vec4 0.0 0.0 0.0 1.0)))
        (fragment (* (sample picture smp (vec2 1.5)) (sample-compare shadow cmp (vec2 -0.5) 0.5))))"
     "Edge.frag.wgsl"))
  (with-check-info (['wgsl wgsl])
    ;; A compare-only sampler has no other variant, but still its properties
    (check-true (string-contains? wgsl "override zgalLinear_cmp: bool;\noverride zgalBorder_cmp: u32;\noverride zgalCompareOp_cmp: u32;"))
    (check-false (string-contains? wgsl "zgalCompareOp_smp"))
    ;; Outside the image, the border colour; inside, the texel
    (check-true (string-contains? wgsl "any(at >= vec2i(textureDimensions(t)))) {\n        return color;\n    }\n    return textureLoad(t, at, 0i);"))
    ;; Without a border, the sampler's own sampling, in the branch the override folds away
    (check-true (string-contains? wgsl "if (border != 0u) {\n        return zgalFilterBorder(t, uv, linear, border);\n    }\n    return textureSample(t, s, uv);"))
    (check-true (string-contains? wgsl "zgalSample(picture, smp, vec2f(1.5f), zgalLinear_smp, zgalBorder_smp)"))
    (check-true (string-contains? wgsl "zgalSampleCompare(shadow, zgalCompare_cmp, vec2f(-0.5f), 0.5f, zgalLinear_cmp, zgalBorder_cmp, zgalCompareOp_cmp)")))
  (define vertex
    (web-shader
     "(graphics-material Edge
        (bindings (layers (array texture)) (smp sampler))
        (outputs (color vec4))
        (vertex (results #:position (sample-lod (at layers 2) smp (vec2 0.5) 0.0)))
        (fragment (vec4 1.0)))"
     "Edge.vert.wgsl"))
  (with-check-info (['wgsl vertex])
    ;; An element of an array is a layer of its bucket, clamped as WebGPU's sampling clamps it
    (check-true (string-contains? vertex "let l = clamp(layer, 0i, i32(textureNumLayers(t)) - 1i);"))
    (check-true (string-contains? vertex "zgalSampleLevel_layers(u32(2i), smp, vec2f(0.5f), 0.0f, zgalLinear_smp, zgalBorder_smp)"))
    (check-true (string-contains? vertex "//zgal-bucket layers case {bucket}u: { return zgalSampleArrayLevel(zgalBucket_layers_{bucket}, s, uv, i32(entry.y), level, linear, border); }"))))

(test-case "An array of textures is buckets the runtime declares, and a table of its elements: operations on an element switch on its bucket"
  (define program
    "(define (texel [t : texture2D] [s : sampler] [k : uint]) : vec4 (fetch t s (ivec2 (int k) 0) 0))
     (graphics-material Show
       (bindings (layers (array texture)) (smp sampler))
       (push (i uint))
       (outputs (color vec4))
       (vertex (results #:position (vec4 0.0 0.0 0.0 1.0)))
       (fragment (+ (sample (at layers i) smp (vec2 0.5))
                    (texel (at layers (+ i (uint 1))) smp (uint (array-length layers)))
                    (vec4 (vec2 (texture-size (at layers 0) 1)) 0.0 0.0))))")
  (define wgsl (web-shader program "Show.frag.wgsl"))
  (define (has text) (check-true (string-contains? wgsl text) text))
  (with-check-info (['wgsl wgsl])
    ;; The table at the array's binding, its count and buckets expanded by the runtime
    (has "\n//zgal-array layers @group(0) @binding(0) var<uniform> zgalTable_layers: array<vec4u, {count}>;\n")
    (has "\n//zgal-array layers const zgalCount_layers = {count}u;\n")
    (has "\n//zgal-bucket layers @group(0) @binding({binding}) var zgalBucket_layers_{bucket}: texture_2d_array<f32>;\n")
    ;; Sampling at the implicit level: the derivatives before the switch, whose control flow isn't
    ;; uniform, then sampling with them
    (has "    let entry = zgalTable_layers[min(e, zgalCount_layers - 1u)];\n    let ddx = dpdx(uv);\n    let ddy = dpdy(uv);\n    switch (entry.x) {\n")
    (has "return textureSampleGrad(t, s, uv, layer, ddx, ddy);")
    (has "zgalSample_layers(zgalPush.i, smp, vec2f(0.5f), zgalLinear_smp, zgalBorder_smp)")
    ;; An element given to a function is its index: the variant switches where it uses it
    (has "fn zgalAt0_layers_texel(t: u32, k: u32) -> vec4f {\n    return zgalFetch_layers(t, vec2i(i32(k), 0i), 0i);\n}")
    (has "zgalAt0_layers_texel(zgalPush.i + 1u, u32(zgalCount_layers))")
    (has "//zgal-bucket layers case {bucket}u: { return textureLoad(zgalBucket_layers_{bucket}, at, i32(entry.y), lod); }")
    ;; Sizes from the table
    (has "return vec2i(max(entry.zw >> vec2u(u32(lod)), vec2u(1u)));")
    (has "zgalSize_layers(u32(0i), 1i)"))
  ;; What can't be done to an element
  (check-not-on-web
   "(compute-material C
      (workgroup 1)
      (bindings (layers (array texture)) (cmp sampler) (counts (buffer-array float)))
      (compute (write! (at counts 0) (sample-compare (at layers 0) cmp (vec2 0.5) 0.5))))"
   "sample-compare can't be used on an element of an array on the web"))

(test-case "An array of storage images is storage buckets of its access the runtime declares, of their formats: stores and loads switch on the bucket"
  (define program
    "(compute-material Copy
       (workgroup 1)
       (bindings (sources (array (storage-image))) (targets (array (storage-image))))
       (compute
        (let* ([at (ivec2 globalInvocationId.xy)]
               [e (remainder globalInvocationId.z (array-length targets))])
          (store! (at targets e) at (+ (image-load (at sources e) at) (vec4 (vec2 (image-size (at sources 0))) 0.0 0.0))))))")
  (define wgsl (web-shader program "Copy.comp.wgsl"))
  (define (has text) (check-true (string-contains? wgsl text) text))
  (with-check-info (['wgsl wgsl])
    ;; Each bucket's format filled in by the runtime, its access the array's (array-length touches
    ;; no element, so targets is write-only)
    (has "\n//zgal-bucket sources @group(0) @binding({binding}) var zgalBucket_sources_{bucket}: texture_storage_2d_array<{format}, read>;\n")
    (has "\n//zgal-bucket targets @group(0) @binding({binding}) var zgalBucket_targets_{bucket}: texture_storage_2d_array<{format}, write>;\n")
    (has "//zgal-bucket sources case {bucket}u: { return textureLoad(zgalBucket_sources_{bucket}, at, i32(entry.y)); }")
    (has "//zgal-bucket targets case {bucket}u: { textureStore(zgalBucket_targets_{bucket}, at, i32(entry.y), value); }")
    ;; A store returns nothing: the switch's default does nothing
    (has "        default: {}\n")
    (has "zgalStore_targets(e, at, zgalLoad_sources(e, at) + vec4f(vec2f(zgalSize_sources(u32(0i), 0i)), 0.0f, 0.0f));"))
  (define ts (web-shader program "test.zgal.ts"))
  (check-true (string-contains? ts "{ name: 'sources', resource: 'sources', kind: 'storage-image-array', access: 'read'") ts)
  (check-true (string-contains? ts "{ name: 'targets', resource: 'targets', kind: 'storage-image-array', access: 'write'") ts))

(test-case "An array of buffers is storage buckets of raw words: accesses switch on the bucket, each bucket's helpers on a bucket line"
  (define program
    "(struct Params (scale float) (tag uint16))
     (compute-material Mix
       (workgroup 4 4)
       (bindings (halves (array (buffer-array uint16))) (counts (array (buffer-array uint))) (params (array (buffer Params))))
       (compute
        (let* ([at globalInvocationId.xy]
               [e (remainder at.y 3)]
               [p (at params e)])
          (write! (at (at halves e) at.x) (uint16 (+ p.scale (float p.tag))))
          (atomic-add! (at (at counts e) 0) 1))))")
  (define wgsl (web-shader program "Mix.comp.wgsl"))
  (define (has text) (check-true (string-contains? wgsl text) text))
  (with-check-info (['wgsl wgsl])
    ;; Elements are words: atomic where atomics act on them or a write changes part of a word
    (has "\n//zgal-bucket params @group(0) @binding({binding}) var<storage, read> zgalBucket_params_{bucket}: array<u32>;\n")
    (has "\n//zgal-bucket halves @group(0) @binding({binding}) var<storage, read_write> zgalBucket_halves_{bucket}: array<atomic<u32>>;\n")
    (has "\n//zgal-bucket counts @group(0) @binding({binding}) var<storage, read_write> zgalBucket_counts_{bucket}: array<atomic<u32>>;\n")
    ;; Each bucket's helpers on one line, which the runtime repeats per bucket
    (has "\n//zgal-bucket params fn zgalLoad_zgalBucket_params_{bucket}_Params(o: u32) -> Params {")
    ;; Elements' accesses switch on the bucket, the element's index clamped to the array
    (has "let entry = zgalTable_params[min(e, zgalCount_params - 1u)];")
    (has "//zgal-bucket params case {bucket}u: { return zgalLoad_zgalBucket_params_{bucket}_Params(o); }")
    (has "//zgal-bucket counts case {bucket}u: { return atomicAdd(&zgalBucket_counts_{bucket}[i], a0); }"))
  (define ts (web-shader program "test.zgal.ts"))
  (check-true (string-contains? ts "{ name: 'halves', resource: 'halves', kind: 'storage-buffer-array', access: 'write'") ts)
  (check-true (string-contains? ts "{ name: 'params', resource: 'params', kind: 'storage-buffer-array', access: 'read'") ts))

(test-case "A graph binds an array of buffers as a BufferArray, uniform elements as their Uniforms"
  (define ts
    (web-shader
      "(struct Params (scale float) (tag uint16))
       (compute-material Read
         (workgroup 1)
         (bindings (params (array (buffer Params))) (result (buffer-array float)))
         (compute (write! (at result 0) (.scale (at params 1)))))
       (graph G
         (input k (per-frame Params))
         (uniform u Params k)
         (buffer c Params 1 #:frames shared)
         (buffer result float 1 #:frames shared)
         (material m Read (params (u c c)) result #:invocations (1))
         (dispatch d m))"
      "test.zgal.ts"))
  (check-true (string-contains? ts "const array_m_params = graph.bufferArray('m\\'s params', [uniform_u, buffer_c, buffer_c]);") ts)
  (check-true (string-contains? ts "params: { buffers: array_m_params }") ts))

(test-case "A sampler clamping to a border clamps to the edge on the web, its border colour beside it"
  (define (sampler options)
    (define ts
      (web-shader
       (string-append corner "(graph G
          (image picture rgba8 (fixed 4 4) #:frames shared)
          (target t (color picture #:clear (0 0 0 1)))
          (sampler s " options ")
          (material m Corner)
          (draw d t m 3))")
       "test.zgal.ts"))
    (cadr (regexp-match #rx"graph.sampler\\('s', ([^\n]*)\\);" ts)))
  (check-regexp-match #rx"addressModeU: 'clamp-to-edge'.*}, 'opaque-black'$" (sampler "#:address clamp-to-border"))
  (check-regexp-match #rx"}, 'opaque-white'$" (sampler "#:address clamp-to-border #:border opaque-white"))
  (check-regexp-match #rx"addressModeU: 'repeat'.*}, null$" (sampler "#:border opaque-white"))
  ;; Natively too, opaque black without #:border, as a float colour (SamplerDesc's default is the
  ;; integer one, undefined on non-integer images)
  (define (native-sampler options)
    (define h
      (for/first ([o (in-list (compile-text (string-append corner "(graph G
          (image picture rgba8 (fixed 4 4) #:frames shared)
          (target t (color picture #:clear (0 0 0 1)))
          (sampler s " options ")
          (material m Corner)
          (draw d t m 3))")))]
                  #:when (string-suffix? (output-path o) ".zgal.h"))
        (output-text o)))
    (cadr (regexp-match #rx"auto sampler_s, VulkanSampler::create\\(context, SamplerDesc{([^}]*)}" h)))
  (check-regexp-match #rx"borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK" (native-sampler "#:address clamp-to-border"))
  (check-false (regexp-match? #rx"borderColor" (native-sampler "#:address clamp-to-edge"))))

(test-case "Only the function variants called are emitted"
  (define wgsl
    (web-shader
     "(define (red [t : texture2D] [s : sampler] [uv : vec2]) : float
        (.x (sample t s uv)))
      ;; Compares its map, so it gives red a depth texture
      (define (shadowed [map : texture2D] [s : sampler] [uv : vec2]) : float
        (* (red map s uv) (sample-compare map s uv 0.5)))
      (compute-material Shade
        (workgroup 1)
        (bindings (shadow texture) (smp sampler) (results (buffer-array float)))
        (compute (write! (at results 0) (shadowed shadow smp (vec2 0.5)))))"
     "Shade.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-true (string-contains? wgsl "fn zgalDepth_0_red(t: texture_depth_2d, zgalDepth_s: sampler, "))
    (check-false (string-contains? wgsl "fn red("))
    (check-regexp-match #rx"fn shadowed\\(map: texture_depth_2d, zgalDepth_s: sampler, zgalCompare_s: sampler_comparison, .*uv: vec2f\\) -> f32" wgsl)
    (check-regexp-match #rx"zgalDepth_0_red\\(map, zgalDepth_s, .*uv\\)" wgsl)))

(test-case "A texture compared in one stage is a depth texture in the others"
  (define wgsl
    (web-shader
     "(graphics-material Lit
        (bindings (shadow texture) (smp sampler))
        (varyings (height float))
        (outputs (color vec4))
        (vertex (results #:position (vec4 0.0 0.0 0.0 1.0) #:height (.x (sample shadow smp (vec2 0.5)))))
        (fragment (vec4 (sample-compare shadow smp (vec2 0.5) height))))"
     "Lit.vert.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-true (string-contains? wgsl "var shadow: texture_depth_2d;"))
    (check-true (string-contains? wgsl "var zgalDepth_smp: sampler;"))
    (check-false (string-contains? wgsl "zgalCompare_smp"))
    (check-true (string-contains? wgsl "override zgalLinear_smp: bool;\noverride zgalBorder_smp: u32;\n"))
    (check-true (string-contains? wgsl "vec4f(zgalSampleDepth(shadow, zgalDepth_smp, vec2f(0.5f), 0i, zgalLinear_smp, zgalBorder_smp), 0.0f, 0.0f, 1.0f).x"))))

(test-case "findMSB is WGSL's firstLeadingBit, its u32 result of a u32 reinterpreted as i32 (-1 for none)"
  (define wgsl
    (web-shader
     "(compute-material Top
        (workgroup 1)
        (bindings (values (buffer-array uint)) (results (buffer-array ivec2)))
        (compute (write! (at results 0) (+ (findMSB (uvec2 (at values 0) 1)) (findMSB (int (at values 1)))))))"
     "Top.comp.wgsl"))
  (with-check-info (['wgsl wgsl])
    (check-regexp-match #rx"vec2i\\(firstLeadingBit\\(vec2u\\(" wgsl)
    (check-regexp-match #rx"[^<(]firstLeadingBit\\(i32\\(" wgsl)))

(test-case "A graphics material's writable bindings mustn't be visible to its vertex stage"
  (define (material bindings vertex fragment)
    (format "(graphics-material Paint
       (outputs (color vec4))
       (bindings ~a)
       (vertex ~a)
       (fragment ~a))" bindings vertex fragment))
  (define position "(results #:position (vec4 0.0 0.0 0.0 1.0))")
  (define (then effect) (format "(begin ~a ~a)" effect position))
  (define reading "(results #:position (vec4 (float (at counts 0)) 0.0 0.0 1.0))")
  (define color "(vec4 1.0)")
  ;; Written by the vertex shader
  (check-not-on-web (material "(counts (buffer-array uint))" (then "(write! (at counts 0) 1)") color)
                    "binding counts is write and its vertex shader uses it")
  (check-not-on-web (material "(counts (buffer-array uint))" (then "(atomic-add! (at counts 0) 1)") color)
                    "binding counts is read-write and its vertex shader uses it")
  (check-not-on-web (material "(marks (storage-image))" (then "(store! marks (ivec2 0 0) (vec4 1.0))") color)
                    "binding marks is write and its vertex shader uses it")
  ;; Read by the vertex shader, written by the fragment shader: one binding, read-write
  (check-not-on-web (material "(counts (buffer-array uint))" reading "(begin (write! (at counts 1) 2) (vec4 1.0))")
                    "binding counts is read-write and its vertex shader uses it")
  ;; Used by no shader, so visible to every stage
  (check-not-on-web (material "(counts (buffer-array uint write))" position color)
                    "binding counts is write, but no shader uses it")
  ;; Written by the fragment shader only, read by the vertex shader only, or unused and read: fine
  (for ([ok (list (material "(counts (buffer-array uint))" position "(begin (write! (at counts 0) 1) (vec4 1.0))")
                  (material "(counts (buffer-array uint))" reading color)
                  (material "(counts (buffer-array uint read))" position color))])
    (check-false (zgal-error-of ok #:web? #t) ok)))
