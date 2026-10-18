#lang racket/base
;; Imports, the standard library, namespaces, and getting data in and out of graphs

(require racket/file
         racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (header text #:source [source "test.zgal"])
  (define-values (_prog outputs) (compile-zgal source #:text text #:header-dir "out" #:shader-dir "out"))
  (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) ".zgal.h")) o)))

(test-case "The standard library is imported by module name, and its functions compile to C++"
  (define h (header "(import camera)\n(import color)\n(define (view [eye : vec3]) : mat4 (lookAt eye (vec3 0.0) (vec3 0.0 1.0 0.0)))"))
  (check-true (string-contains? h "inline glm::mat4 lookAt("))
  (check-true (string-contains? h "inline glm::vec3 acesFilmic("))
  ;; Locations name the module's file, relative to the working directory (the test's)
  (check-true (string-contains? h "#line 7 \"../lib/camera.zgal\""))
  (check-zgal-error "(import geometry)" 'bad-import "has no module geometry; it has camera, color, indirect, math, random, sorting"))

(test-case "Files import files, once each, and not themselves"
  (define directory (make-temporary-directory))
  (define (write! name text) (call-with-output-file (build-path directory name) (λ (o) (write-string text o)) #:exists 'truncate))
  (write! "shapes.zgal" "(import \"units.zgal\")\n(struct Circle (radius float))\n(define (area [c : Circle]) : float (* c.radius c.radius 3.14))")
  (write! "units.zgal" "(define (scaled [x : float]) : float (* x 2.0))")
  (write! "program.zgal" "(import \"shapes.zgal\")\n(import \"units.zgal\")\n(define (twice [c : Circle]) : float (scaled (area c)))")
  (define main (path->string (build-path directory "program.zgal")))
  (define-values (prog _) (compile-zgal main #:check-only? #t))
  (check-true (string-contains? (header (file->string main) #:source main) "inline float scaled("))
  (write! "loop.zgal" "(import \"program.zgal\")")
  (write! "program.zgal" "(import \"loop.zgal\")")
  (check-exn #rx"imports itself" (λ () (compile-zgal main #:check-only? #t)))
  ;; Materials, graphs and apps too, which the importing file compiles (in its namespace); a main
  ;; is a program's own
  (write! "program.zgal" "(import \"material.zgal\")\n(main A (window \"a\" 4 4) (timestep #:max 1/30 #:fixed 1/60))")
  (write! "material.zgal" (string-append "(compute-material C (workgroup 1) (bindings (b (buffer-array uint))) (compute (write! (at b 0) 1)))\n"
                                         "(graph G (buffer b uint 1 #:frames shared) (material c C b #:invocations (1)) (dispatch d c))\n"
                                         "(app A (state (n uint 0)) (on (frame dt) (with state #:n (+ state.n 1))) (run G))"))
  (define-values (_with-material outputs) (compile-zgal main #:header-dir "out" #:shader-dir "out"))
  (check-true (for/or ([o (in-list outputs)]) (string-suffix? (output-path o) "C.comp")))
  (define imported-header (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) "program.zgal.h")) o)))
  (check-true (string-contains? imported-header "namespace program {"))
  (check-true (string-contains? imported-header "struct G {"))
  (check-true (string-contains? imported-header "class A {"))
  (write! "material.zgal" "(main A)")
  (check-exn #rx"declares structs, vertex formats, functions, constants, GLSL functions, materials, graphs, apps and imports, not [(]main" (λ () (compile-zgal main #:check-only? #t)))
  ;; By absolute path
  (write! "program.zgal" (format "(import ~s)" (path->string (build-path directory "units.zgal"))))
  (check-true (string-contains? (header (file->string main) #:source main) "inline float scaled("))
  (delete-directory/files directory))

(test-case "Generated C++ is in the namespace of its file, which must be able to name one"
  (define h (header "(struct A (x float))"))
  (check-true (string-contains? h "namespace test {"))
  (check-true (string-contains? h "} // namespace test"))
  (check-exn #rx"names its generated C\\+\\+'s namespace" (λ () (header "(struct A (x float))" #:source "my-file.zgal")))
  (check-exn #rx"names its generated C\\+\\+'s namespace" (λ () (header "(struct A (x float))" #:source "class.zgal"))))

(define data-graph
  (string-append
   "(graph G\n"
   "  (input values (data uint))\n"
   "  (upload stored values)\n"
   "  (buffer copied uint (count values) #:frames shared)\n"
   "  (image picture rgba8 (fixed 2 2) #:frames shared)\n"
   "  (copy toCopied stored copied)\n"
   "  (copy toPicture stored picture)\n"
   "  (clear zero copied #:value 3)\n"
   "  (readback readCopied copied)\n"
   "  (readback readPicture picture))\n"))

(test-case "Data inputs are uploaded into buffers, which copies, clears and readbacks use"
  (define h (header data-graph))
  (check-true (string-contains? h "std::vector<uint32_t> values;"))
  (check-true (string-contains? h "TRY_ASSIGN(auto upload_stored, zgal::upload(context, in->values, \"G's upload stored\"));"))
  (check-true (string-contains? h "static_cast<uint32_t>(in->values.size())"))
  (check-true (string-contains? h "zgal::copyBufferToImage(record, source, destination, sourceOffset, offset, extent, \"toPicture\")"))
  (check-true (string-contains? h "vkCmdFillBuffer(record.commandBuffer, record.graph.buffer(target, record.frameIndex), 0, VK_WHOLE_SIZE, value)"))
  (check-true (string-contains? h "Result<std::vector<uint32_t>> readCopiedContents(RenderGraph& graph) const"))
  (check-true (string-contains? h "Result<std::vector<uint8_t>> readPicturePixels(RenderGraph& graph) const"))
  ;; Copies into images take a region: an offset and an extent in texels, by default all of it
  (check-true (string-contains? (header (string-replace data-graph "(copy toPicture stored picture)"
                                                        "(copy toPicture stored picture #:source-offset 2 #:destination-offset (1 0) #:extent (1 2))"))
                                "sourceOffset = VkDeviceSize(2u) * sizeof(uint32_t), offset = VkOffset2D{int32_t(1u), int32_t(0u)}, extent = std::optional<VkExtent2D>(VkExtent2D{1u, 2u})"))
  (check-zgal-error (string-replace data-graph "(copy toPicture stored picture)" "(copy toPicture stored picture #:count 2)")
                    'bad-copy "whose region is an #:extent (width height), not a #:count")
  (check-zgal-error (string-replace data-graph "(copy toCopied stored copied)" "(copy toCopied stored copied #:extent (1 1))")
                    'bad-copy "take a #:count of elements, not an #:extent")
  (check-zgal-error (string-replace data-graph "(copy toPicture stored picture)" "(copy toPicture stored picture #:destination-offset 3)")
                    'bad-copy "#:destination-offset is (x y), in texels")
  (check-zgal-error "(graph G (input n (per-frame uint)) (upload u n))" 'type-mismatch "n is a per-frame input, not a data input")
  (check-zgal-error "(graph G (input n (per-frame uint)) (constant c (count n)))" 'type-mismatch "(count input) takes a data input"))

(test-case "Apps call extern functions C++ gives, which can't shadow the file's"
  (define h (header "(app A (extern (measure [i : uint]) : float) (state (x float 0.0)) (on (key-pressed a) (with state #:x (measure 3))))"))
  (check-true (string-contains? h "std::function<float(uint32_t)> measure;"))
  (check-true (string-contains? h "float measure(uint32_t i) { return inputs_.measure(i); }"))
  (check-true (string-contains? h "zgalUpdated1.x = measure(3u);"))
  (check-zgal-error "(define (f [x : float]) : float x)\n(app A (extern (f [x : float]) : float))" 'duplicate-name "already a function of the file"))

(test-case "GLSL functions are included in the shaders using them, and only shaders can call them"
  (define text (string-append "(extern-glsl \"noise.glsl\" ((noise [p : vec2]) : float))\n"
                              "(define (grain [p : vec2]) : float (* 0.5 (noise p)))\n"
                              "(compute-material Fill (workgroup 1) (bindings (result (buffer float)))\n"
                              "  (compute (write! result (grain (vec2 1.0)))))\n"))
  (define outputs (compile-text text #:bodies (hash "noise.glsl" "float noise(vec2 p) {\n    return p.x;\n}\n")))
  (define (named suffix) (output-text (for/first ([o (in-list outputs)] #:when (string-suffix? (output-path o) suffix)) o)))
  (check-true (string-contains? (named "Fill.comp") "#line 1 \"noise.glsl\"\nfloat noise(vec2 p) {"))
  (check-false (string-contains? (named ".zgal.h") "grain"))
  (check-zgal-error (string-append text "(app A (state (x float 0.0)) (on (key-pressed a) (with state #:x (grain (vec2 0.0)))))")
                    'gpu-only "grain doesn't compile to C++")
  (check-zgal-error "(extern-glsl \"missing.glsl\" ((f [x : float]) : float))\n(compute-material C (workgroup 1) (bindings (o (buffer float))) (compute (write! o (f 1.0))))"
                    'shader-not-found "GLSL file missing.glsl doesn't exist"))
