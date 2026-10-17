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
