#lang racket/base
;; What compiling for the web reports that Vulkan allows (not-on-web): what WebGPU can't do, and what
;; the web backend doesn't do yet; and how it compiles what some WebGPU implementations refuse

(require racket/list
         racket/string
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
