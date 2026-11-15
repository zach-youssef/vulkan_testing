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
   "(compute-material Pick
      (workgroup 1)
      (bindings (sources (array (buffer-array uint))) (counts (buffer-array uint)))
      (compute (write! (at counts 0) (array-length sources))))"
   "can't be an array of buffer-arrays")
  (check-not-on-web
   "(struct Pair (a uint) (b uint))
    (compute-material Fill
      (workgroup 1)
      (bindings (values (buffer-array uint)))
      (push (pair Pair))
      (compute (write! (at values 0) pair.a)))"
   "push"))

(test-case "Graphs using what WebGPU lacks"
  (check-not-on-web
   (string-append corner
                  "(graph G
                     (image picture rgba8 (fixed 4 4) #:frames shared)
                     (target t (color picture #:clear (0 0 0 1)))
                     (sampler border #:address clamp-to-border)
                     (material m Corner)
                     (draw d t m 3))")
   "border"))

(test-case "Graphs binding arrays of textures to what isn't copied into a texture array"
  (define (graph . items)
    (string-append layered "(graph G\n  (buffer counts uint 1 #:frames shared)\n  " (string-join items "\n  ")
                   "\n  (dispatch d m #:invocations (1)))"))
  (check-not-on-web (graph "(image a rgba8 (fixed 4 4) #:frames shared)" "(image b rgba8 (fixed 4 4) #:frames shared)"
                           "(material m Layered (layers (a b)) counts)")
                    "only image files' (and models') images")
  (check-not-on-web (graph "(image-file a \"a.png\" #:color srgb)" "(image-file b \"b.png\" #:color linear)"
                           "(material m Layered (layers (a b)) counts)")
                    "the same #:color")
  (check-not-on-web (graph "(import-images given #:resting shader-read-only)" "(material m Layered (layers given) counts)")
                    "can't be imported"))

;; The WGSL compiling text for the web gives for shader `name` (e.g. "Stamp.comp.wgsl")
(define (web-shader text name)
  (define-values (_prog outputs)
    (compile-zgal "test.zgal" #:text text #:read-body (λ (path) #f) #:native? #f #:web-dir "out/web"))
  (output-text (findf (λ (o) (string-suffix? (output-path o) name)) outputs)))

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
    (check-true (string-contains? wgsl "textureStore(img, vec2i(0i, 0i), textureSampleLevel(tex, smp, "))))

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
  (check-refused "(struct P (a uint))\n(compute-material C (shaders \"c.comp\") (workgroup 1) (push (p P)))"
                 "push constant"))
