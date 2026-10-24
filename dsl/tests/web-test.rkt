#lang racket/base
;; What compiling for the web reports that Vulkan allows (not-on-web): what WebGPU can't do, and what
;; the web backend doesn't do yet

(require racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

;; Checks compiling text for the web reports not-on-web, with a message containing fragment
(define (check-not-on-web text fragment)
  (define e (zgal-error-of text #:web? #t))
  (with-check-info (['program text])
    (check-true (exn:zgal? e))
    (when (exn:zgal? e)
      (check-equal? (exn:zgal-id e) 'not-on-web)
      (check-true (string-contains? (exn-message e) fragment) (exn-message e)))))

(define fill
  "(compute-material Fill
     (workgroup 1)
     (bindings (values (buffer-array uint)))
     (compute (write! (at values 0) 1)))\n")

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
                     (material m Corner #:cull front-and-back)
                     (draw d t m 3))")
   "cull both faces")
  (check-not-on-web
   (string-append corner
                  "(graph G
                     (image picture rgba8 (fixed 4 4) #:frames shared)
                     (target t (color picture #:clear (0 0 0 1)))
                     (sampler border #:address clamp-to-border)
                     (material m Corner)
                     (draw d t m 3))")
   "border")
  (check-not-on-web
   (string-append fill
                  "(graph G
                     (input write cpu)
                     (import-buffer staging uint)
                     (cpu c write #:writes (staging)))")
   "maps buffers"))

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
