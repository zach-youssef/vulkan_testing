#lang racket/base
;; ZGAL's errors: the catalog (which the documentation renders) against what the compiler reports

(require racket/file
         racket/list
         racket/runtime-path
         racket/set
         racket/string
         rackunit
         "../compile.rkt"
         "../private/errors.rkt"
         "util.rkt")

(define-runtime-path dsl-directory "..")

(test-case "Every catalogued error's example reports it, at a location"
  (for ([k (in-list error-catalog)])
    (with-check-info (['id (error-kind-id k)])
      (define e (zgal-error-of (error-kind-example k) #:bodies (error-kind-bodies k)))
      (check-true (exn:zgal? e) "the example compiles")
      (when (exn:zgal? e)
        (check-equal? (exn:zgal-id e) (error-kind-id k) (exn-message e))
        (check-true (loc? (exn:zgal-where e)))))))

(test-case "The catalog lists every error the compiler raises, and nothing else"
  (define sources
    (append (directory-list (build-path dsl-directory "private") #:build? #t)
            (list (build-path dsl-directory "compile.rkt"))))
  (define raised
    (for*/set ([file (in-list sources)]
               #:when (regexp-match? #rx"[.]rkt$" (path->string file))
               [m (in-list (regexp-match* #px"zgal-error '([a-z-]+)" (file->string file) #:match-select cadr))])
      (string->symbol m)))
  (define catalogued (for/set ([k (in-list error-catalog)]) (error-kind-id k)))
  (check-equal? (set-subtract raised catalogued) (set) "raised but not catalogued")
  (check-equal? (set-subtract catalogued raised) (set) "catalogued but never raised")
  (check-equal? (length error-catalog) (set-count catalogued) "ids are unique"))

(test-case "Errors are reported where they are, in the compiler's format"
  (define e (zgal-error-of "(struct A (x float))\n(struct B\n  (y float3))"))
  (check-equal? (format-zgal-error e) "test.zgal:3:6: error: unknown type float3 [unknown-type]"))

(test-case "Read errors report the reader's message"
  (check-zgal-error "(struct A (x float)" 'read-error "expected a `)`"))

(test-case "Programs can't run code through the reader"
  (check-zgal-error "#reader racket/base (exit 3)" 'read-error "#reader"))

(test-case "Lisp-style names get a suggestion"
  (check-zgal-error "(struct A (shadow-map-index uint))" 'bad-identifier "write shadowMapIndex"))

(test-case "Names can't be GLSL or C++ keywords, built-ins, wrapper names or generated names"
  (check-zgal-error "(struct A (texture uint))" 'reserved-name "GLSL built-in")
  (check-zgal-error "(struct A (output uint))" 'reserved-name "keyword")
  (check-zgal-error "(struct A (template uint))" 'reserved-name "keyword")
  (check-zgal-error "(struct Material (x uint))" 'reserved-name "wrapper")
  (check-zgal-error "(struct A (gl_Thing uint))" 'reserved-name "gl_")
  (check-zgal-error "(struct A (zgalData uint))" 'reserved-name "zgal")
  (check-zgal-error "(struct A (materialPush uint))" 'reserved-name "generated code")
  (check-zgal-error "(struct A (a__b uint))" 'bad-identifier "__"))

(test-case "Material names share the shaders' global scope, and structs' names"
  (check-zgal-error "(struct A (x float))\n(struct A (y float))" 'duplicate-name "already declared")
  (check-zgal-error
   "(struct Light (x float))\n(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (Light texture)))"
   'duplicate-name "already declared as a struct")
  (check-zgal-error
   (string-append "(vertex-format V (color vec3))\n"
                  "(graphics-material G (shaders \"g.vert\" \"g.frag\") (vertex-input V)\n"
                  "  (varyings (inColor vec3)) (outputs (o vec4)))")
   'duplicate-name "inColor"))

(test-case "Structs must be declared before they're used"
  (check-zgal-error "(struct A (b B))\n(struct B (x float))" 'unknown-type "used before it's declared"))

(test-case "Buffer bindings hold a single value; buffer-array bindings arrays of them"
  (check-zgal-error
   "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (b (buffer (array float 4) read))))"
   'type-not-allowed "buffer-array")
  (check-zgal-error
   "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (b (buffer-array mat3 read))))"
   'type-not-allowed "mat4"))

(test-case "Accesses are required and checked"
  (check-zgal-error
   "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (image (storage-image))))"
   'bad-binding "(storage-image access)")
  (check-zgal-error
   "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (image (storage-image readwrite))))"
   'bad-binding "read, write or read-write"))

(test-case "Stages are checked"
  (check-zgal-error
   "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings (t texture #:stages (compute))))"
   'bad-stages "compute materials")
  (check-zgal-error
   "(graphics-material G (shaders \"g.vert\" \"g.frag\") (bindings (s sampler #:stages (fragment))) (outputs))"
   'bad-stages "samplers")
  (check-zgal-error
   "(graphics-material G (shaders \"g.vert\" \"g.frag\") (bindings (t texture #:stages (vertex vertex))) (outputs))"
   'duplicate-name "vertex"))

(test-case "Shaders are one per stage, and belong to one material"
  (check-zgal-error
   "(graphics-material G (shaders \"g.vert\" \"g.comp\") (outputs))" 'bad-shader "a .vert and a .frag")
  (check-zgal-error "(compute-material C (shaders \"c.glsl\") (workgroup 1))" 'bad-shader ".comp")
  ;; The shader library finds shaders by file name, so different directories don't help
  (check-zgal-error
   (string-append "(compute-material A (shaders \"a/c.comp\") (workgroup 1))\n"
                  "(compute-material B (shaders \"b/c.comp\") (workgroup 1))")
   'shader-reused "c.comp"))

(test-case "Push constants are limited to 128 bytes, binding indices included"
  ;; 31 indices and a float fit; 32 indices and a float don't
  (define (program bindings)
    (format "(compute-material C (shaders \"c.comp\") (workgroup 1) (bindings ~a) (push (x float)))"
            (string-join (for/list ([i (in-range bindings)]) (format "(t~a texture)" i)))))
  (check-false (zgal-error-of (program 31) #:bodies (hash "c.comp" "void main() {}")))
  (check-zgal-error (program 32) 'push-too-large "132 bytes"))

(test-case "Integer varyings must be flat"
  (check-false (zgal-error-of
                "(graphics-material G (shaders \"g.vert\" \"g.frag\") (varyings (id uint flat)) (outputs))"
                #:bodies (hash "g.vert" "void main() {}" "g.frag" "void main() {}")))
  (check-zgal-error
   "(graphics-material G (shaders \"g.vert\" \"g.frag\") (varyings (id ivec2 noperspective)) (outputs))"
   'varying-not-flat "(id ivec2 flat)"))

(test-case "Shader bodies don't declare a version, and are reported at their own lines"
  (define e (zgal-error-of "(compute-material C (shaders \"c.comp\") (workgroup 1))"
                            #:bodies (hash "c.comp" "// A comment\n  #version 460\nvoid main() {}")))
  (check-equal? (exn:zgal-id e) 'version-in-body)
  (check-equal? (exn:zgal-where e) (loc "c.comp" 2 1)))

;; Graphs

(define fill-material
  (string-append "(struct Data (x float))\n"
                 "(compute-material Fill (shaders \"fill.comp\") (workgroup 64)\n"
                 "  (bindings (data (buffer Data read-write))) (push (scale float)))\n"))

(test-case "Graph names refer to declarations before them"
  (check-zgal-error "(graph G\n  (constant a b)\n  (constant b 1))" 'unknown-name "b isn't declared in this graph before here"))

(test-case "Integer literals take their context's type, and other mismatches are reported"
  (check-false (zgal-error-of "(graph G\n  (parameter n uint)\n  (constant total (* 6 n))\n  (image i rgba8 (fixed total 2) #:frames shared))"))
  (check-zgal-error "(graph G\n  (parameter n uint)\n  (constant x (+ n 1.5)))" 'type-mismatch "the operands of +: uint and float don't match")
  (check-zgal-error "(graph G\n  (parameter n uint)\n  (constant x (if n 1 2)))" 'type-mismatch "a condition is a bool, not a uint"))

(test-case "Per-frame inputs are only for push constants"
  (check-zgal-error "(graph G\n  (input on (per-frame bool))\n  (image i rgba8 (fixed 4 4) #:frames shared #:when on))"
                    'not-build-time "on is a per-frame input"))

(test-case "Uniforms hold their source's type, and shaders only read them"
  (check-zgal-error (string-append fill-material "(graph G\n  (input source (per-frame float))\n  (uniform u Data source))")
                    'type-mismatch "source is a per-frame float, but the uniform holds a Data")
  (check-zgal-error (string-append fill-material
                                   "(graph G\n  (input source (per-frame Data))\n  (uniform u Data source)\n"
                                   "  (material fill Fill (data u) #:push ((scale 1.0)) #:invocations (1))\n  (dispatch run fill))")
                    'binding-mismatch "u is a uniform, which the CPU writes and shaders only read"))

(test-case "Buffers bound to materials hold the bindings' types"
  (check-zgal-error (string-append fill-material
                                   "(graph G\n  (buffer b float 4 #:frames shared)\n"
                                   "  (material fill Fill (data b) #:push ((scale 1.0)) #:invocations (1))\n  (dispatch run fill))")
                    'binding-mismatch "binding data holds Data, but b holds float"))

(test-case "Compute materials in graphs say how many invocations they dispatch"
  (check-zgal-error (string-append fill-material
                                   "(graph G\n  (buffer b Data 4 #:frames shared)\n"
                                   "  (material fill Fill (data b) #:push ((scale 1.0)))\n  (dispatch run fill))")
                    'bad-option "needs #:invocations"))

(test-case "draw-meshes needs a material with vertex input, and draw one without"
  (check-zgal-error (string-append "(graphics-material Flat (shaders \"f.vert\" \"f.frag\") (outputs (color vec4)))\n"
                                   "(graph G\n  (input meshes meshes)\n  (target t (surface #:depth none))\n"
                                   "  (material quad Flat)\n  (draw-meshes run t meshes quad))")
                    'material-kind "has no vertex-input"))

(test-case "Push constants' values have the fields' types"
  (check-zgal-error (string-append fill-material
                                   "(graph G\n  (buffer b Data 4 #:frames shared)\n"
                                   "  (material fill Fill (data b) #:push ((scale #t)) #:invocations (1))\n  (dispatch run fill))")
                    'type-mismatch "push constant scale is a float, not a bool"))

(test-case "Graph images' sizes are known each frame, imported images' when the graph is built"
  (check-zgal-error "(graph G\n  (image a rgba8 (surface 1.0) #:frames shared)\n  (constant w (width a)))"
                    'not-build-time "a is a graph image, whose size is only known each frame")
  (check-false (zgal-error-of "(graph G\n  (import-image a #:resting general)\n  (constant w (width a)))")))

(test-case "Compute materials' invocations can use per-frame inputs and images' sizes"
  (check-false (zgal-error-of
                (string-append fill-material
                               "(graph G\n  (input n (per-frame uint))\n  (image i rgba8 (surface 1.0) #:frames shared)\n"
                               "  (buffer b Data 4 #:frames shared)\n"
                               "  (material fill Fill (data b) #:push ((scale 1.0)) #:invocations ((+ n (width i))))\n"
                               "  (dispatch run fill))")
                #:bodies (hash "fill.comp" "void main() {}"))))
