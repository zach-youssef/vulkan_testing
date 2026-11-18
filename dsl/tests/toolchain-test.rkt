#lang racket/base
;; Generated code through the real tools: glslc and the C++ compiler, with the build's flags (the
;; Makefile's test-dsl target passes them in ZGAL_GLSLC, ZGAL_GLSLC_FLAGS, ZGAL_CXX and
;; ZGAL_CXXFLAGS), Dawn (WebGPU, under Node: ZGAL_NODE and ZGAL_WEB, the web directory with its
;; packages installed), naga (ZGAL_NAGA, with glslc and Dawn: GLSL bodies to WGSL), and the command
;; line

(require racket/file
         racket/list
         racket/path
         racket/port
         racket/runtime-path
         racket/set
         racket/string
         racket/system
         rackunit
         "../compile.rkt"
         "../private/ast.rkt"
         "../private/errors.rkt"
         "../private/web-glsl.rkt"
         "../private/wgsl.rkt"
         "golden-test.rkt"
         "util.rkt")

(define-runtime-path zgal.rkt "../zgal.rkt")
(define-runtime-path repository "../..")

;; Runs a program, returning its exit code and what it wrote to stdout and stderr (read from one
;; port: reading them one after the other blocks once the other pipe fills, e.g. glslc's errors)
(define (run program . arguments)
  (define-values (process out in _err)
    (apply subprocess #f #f 'stdout (find-executable-path* program) arguments))
  (close-output-port in)
  (define output (port->string out))
  (subprocess-wait process)
  (close-input-port out)
  (values (subprocess-status process) output))

(define (find-executable-path* program)
  (or (if (absolute-path? program) program (find-executable-path program))
      (error 'run "can't find ~a" program)))

(define (flags name) (string-split (or (getenv name) "")))

(define (write-outputs outputs directory)
  (for ([o (in-list outputs)])
    (define file (build-path directory (file-name-from-path (output-path o))))
    (call-with-output-file file (λ (out) (void (write-string (output-text o) out))) #:exists 'truncate/replace)))

(define-syntax-rule (with-temporary-directory (directory) body ...)
  (let ([directory (make-temporary-directory "zgal-test-~a")])
    (dynamic-wind void (λ () body ...) (λ () (delete-directory/files directory)))))

(define (when-tool variable thunk)
  (if (getenv variable)
      (thunk)
      (printf "Skipping ~a: ~a isn't set (make test-dsl sets it)\n" (object-name thunk) variable)))

(define (glslc-tests)
  (test-case "Every generated shader compiles"
    (with-temporary-directory (directory)
      (define outputs (golden-outputs))
      (write-outputs outputs directory)
      (for ([o (in-list outputs)]
            #:unless (regexp-match? #rx"[.](h|main[.]cpp)$" (output-path o)))
        (define name (path->string (file-name-from-path (output-path o))))
        (define-values (status text)
          (apply run (getenv "ZGAL_GLSLC")
                 (append (flags "ZGAL_GLSLC_FLAGS")
                         (list (path->string (build-path directory name)) "-o"
                               (path->string (build-path directory (string-append name ".spv")))))))
        (with-check-info (['shader name] ['glslc text])
          (check-equal? status 0)))))

  (test-case "glslc reports errors in a shader body at the body's own lines"
    (with-temporary-directory (directory)
      (define outputs
        (compile-text "(compute-material C (shaders \"shaders/broken.comp\") (workgroup 1) (push (x float)))"
                      #:source "check/c.zgal"
                      #:bodies (hash "check/shaders/broken.comp" "void main() {\n    float y = x() + missing;\n}\n")))
      (write-outputs outputs directory)
      (define-values (status text)
        (apply run (getenv "ZGAL_GLSLC")
               (append (flags "ZGAL_GLSLC_FLAGS")
                       (list (path->string (build-path directory "broken.comp")) "-o"
                             (path->string (build-path directory "broken.spv"))))))
      (check-not-equal? status 0)
      (check-regexp-match #rx"check/shaders/broken[.]comp:2: error: 'missing'" text))))

(define (dawn-tests)
  (test-case "Every generated WGSL module compiles: the golden ones, the examples' and the tests'"
    (with-temporary-directory (directory)
      ;; The examples' graphs use what the web doesn't have yet (image files, models), so only
      ;; their materials' shaders are compiled
      (define outputs
        (append (filter (λ (o) (regexp-match? #rx"[.]wgsl$" (output-path o))) (golden-web-outputs))
                (for*/list ([source (in-list (list "examples/image_filter/image_filter.zgal"
                                                   "examples/particles/particles.zgal"
                                                   "examples/scene/scene.zgal"))]
                            [prog (in-value (let-values ([(prog _outputs)
                                                          (parameterize ([current-directory repository])
                                                            (compile-zgal source #:native? #f #:check-only? #t))])
                                              prog))]
                            [m (in-list (program-materials prog))]
                            [s (in-list (material-shaders m))])
                  (output (wgsl-shader-file-name s) (emit-wgsl prog m s)))))
      (write-outputs outputs directory)
      (define-values (status text)
        (apply run (getenv "ZGAL_NODE")
               (path->string (build-path (getenv "ZGAL_WEB") "tools" "check-wgsl.ts"))
               (for/list ([o (in-list outputs)]) (path->string (build-path directory (file-name-from-path (output-path o)))))))
      (with-check-info (['dawn text])
        (check-equal? status 0))))

  (test-case "The golden TypeScript type checks beside the runtime"
    (with-temporary-directory (directory)
      (define web (getenv "ZGAL_WEB"))
      (write-outputs (filter (λ (o) (regexp-match? #rx"[.]ts$" (output-path o))) (golden-web-outputs)) directory)
      (copy-directory/files (build-path web "zgal") (build-path directory "zgal"))
      (make-file-or-directory-link (build-path web "node_modules") (build-path directory "node_modules"))
      (call-with-output-file (build-path directory "package.json") (λ (out) (write-string "{\"type\": \"module\"}" out)))
      ;; The runtime's settings, for the generated module
      (call-with-output-file (build-path directory "tsconfig.json")
        (λ (out) (write-string (regexp-replace #rx"\"include\": [^\n]*" (file->string (build-path web "tsconfig.json")) "\"include\": [\"*.ts\"]") out)))
      (define-values (status text)
        (run (getenv "ZGAL_NODE") (path->string (build-path web "node_modules" "typescript" "bin" "tsc"))
             "-p" (path->string directory)))
      (with-check-info (['tsc text])
        (check-equal? status 0)))))

;; Runs check-wgsl.ts (Dawn) on the files, returning its exit code and output
(define (check-wgsl files)
  (apply run (getenv "ZGAL_NODE")
         (path->string (build-path (getenv "ZGAL_WEB") "tools" "check-wgsl.ts"))
         (map path->string files)))

;; GLSL bodies to WGSL (web-glsl.rkt), through glslc and naga, with the WGSL checked by Dawn
(define (naga-tests)
  (define toolchain (web-toolchain (getenv "ZGAL_GLSLC") (getenv "ZGAL_NAGA") '()))
  (define (storage-format b) "rgba8")

  (test-case "The golden GLSL bodies become WGSL modules Dawn accepts, each using the bindings its stage reads"
    (with-temporary-directory (directory)
      (define-values (prog _outputs)
        (parameterize ([current-directory repository]) (compile-zgal golden-source #:check-only? #t)))
      (define results
        (for*/list ([m (in-list (program-materials prog))] [s (in-list (material-shaders m))] #:when (shader-path s))
          (define body (file->string (build-path repository (shader-path s))))
          (define glsl (emit-web-shader prog m s body (shader-name s) #:storage-format storage-format))
          (cons (shader-name s) (web-shader->wgsl toolchain s glsl))))
      (check-equal? (for/list ([r (in-list results)]) (cons (car r) (sort (set->list (web-wgsl-used (cdr r))) <)))
                    '(("surface.vert" 1 5) ("surface.frag" 0 2 3 4 5) ("accumulate.comp" 0 1 2) ("plain.vert") ("plain.frag")))
      (for ([r (in-list results)])
        (check-equal? (web-wgsl-warnings (cdr r)) "" (car r)))
      (define frag (web-wgsl-text (cdr (assoc "surface.frag" results))))
      (with-check-info (['wgsl frag])
        ;; The bind group as the WGSL backend lays it out: bindings at their indices, read-only
        ;; buffers read, storage images with their format and access, the push constants a uniform
        (check-regexp-match #rx"@binding\\(0\\) *\n *var<storage> zgal_lights" frag)
        (check-regexp-match #rx"@binding\\(4\\) *\n *var zgal_mask: texture_storage_2d<rgba8unorm, *read>" frag)
        (check-regexp-match #rx"@binding\\(5\\) *\n *var<uniform> materialPush: ZgalPush" frag)
        (check-regexp-match #rx"@fragment" frag))
      (define files
        (for/list ([r (in-list results)])
          (define file (build-path directory (string-append (car r) ".wgsl")))
          (call-with-output-file file (λ (out) (void (write-string (web-wgsl-text (cdr r)) out))))
          file))
      (define-values (status text) (check-wgsl files))
      (with-check-info (['dawn text])
        (check-equal? status 0))))

  (test-case "A readonly buffer is read in naga's WGSL though glslc decorated only its block's members NonWritable"
    ;; Ubuntu 24.04's glslc (shaderc 2023.8) leaves out the variable's NonWritable that newer ones
    ;; add: strip it from this glslc's module to get that one's
    (with-temporary-directory (directory)
      (define source (build-path directory "r.comp"))
      (call-with-output-file source
        (λ (out) (write-string "#version 460\nlayout(local_size_x = 1) in;\nlayout(std430, set = 0, binding = 0) readonly buffer In { float data[]; } values;\nlayout(std430, set = 0, binding = 1) buffer Out { float data[]; } results;\nvoid main() { results.data[0] = values.data[0]; }\n" out)))
      (define spv (build-path directory "r.spv"))
      (define-values (glslc-status glslc-output)
        (run (getenv "ZGAL_GLSLC") "--target-env=vulkan1.3" "--target-spv=spv1.5" (path->string source) "-o" (path->string spv)))
      (check-equal? glslc-status 0 glslc-output)
      (define module (file->bytes spv))
      (define stripped
        (let loop ([i 20] [kept (list (subbytes module 0 20))])
          (cond
            [(>= i (bytes-length module)) (apply bytes-append (reverse kept))]
            [else
             (define (word k) (integer-bytes->integer module #f #f (+ i (* 4 k)) (+ i (* 4 k) 4)))
             (define size (* 4 (arithmetic-shift (word 0) -16)))
             (loop (+ i size)
                   (if (and (= (word 0) (bitwise-ior (arithmetic-shift 3 16) 71)) (= (word 2) 24))
                       kept
                       (cons (subbytes module i (+ i size)) kept)))])))
      (check-true (< (bytes-length stripped) (bytes-length module)) "glslc decorated no variable NonWritable")
      (define (naga-wgsl bytes)
        (define file (build-path directory "in.spv"))
        (define wgsl (build-path directory "out.wgsl"))
        (call-with-output-file file #:exists 'truncate/replace (λ (out) (void (write-bytes bytes out))))
        (define-values (status output) (run (getenv "ZGAL_NAGA") (path->string file) (path->string wgsl)))
        (check-equal? status 0 output)
        (file->string wgsl))
      ;; Without the variable's decoration naga declares it read_write; marked, read
      (check-regexp-match #rx"var<storage, read_write> values" (naga-wgsl stripped))
      (define marked (naga-wgsl (spirv-mark-readonly-buffers stripped)))
      (check-regexp-match #rx"var<storage> values" marked)
      (check-regexp-match #rx"var<storage, read_write> results" marked)
      ;; A module glslc already marked is kept as it is
      (check-equal? (spirv-mark-readonly-buffers module) module)))

  ;; The first material's first shader of text, compiled with body
  (define (translate text body)
    (define-values (prog _outputs) (compile-zgal "check/c.zgal" #:text text #:check-only? #t))
    (define m (first (program-materials prog)))
    (define s (first (material-shaders m)))
    (web-shader->wgsl toolchain s (emit-web-shader prog m s body (shader-name s) #:storage-format storage-format)))
  (define (translation-error text body)
    (with-handlers ([exn:zgal? values]) (translate text body) #f))

  (test-case "glslc's errors in a GLSL body are reported at the body's line, with how many there are"
    (define e (translation-error "(compute-material C (shaders \"shaders/broken.comp\") (workgroup 1) (push (x float)))"
                                 "void main() {\n    float y = x() + missing;\n    float z = w;\n}\n"))
    (check-true (exn:zgal? e))
    (when (exn:zgal? e)
      (check-equal? (exn:zgal-id e) 'glsl-error)
      (check-equal? (exn:zgal-where e) (loc "check/shaders/broken.comp" 2 1))
      (check-regexp-match #rx"^'missing' : undeclared identifier [(]glslc reports 2 errors in all[)]$" (exn-message e))))

  (test-case "glslc's errors beyond what a pipe holds are read, not waited on"
    (define e (translation-error "(compute-material C (shaders \"shaders/big.comp\") (workgroup 1))"
                                 (string-append "void main() {\n"
                                                (string-append* (for/list ([i (in-range 3000)]) (format "    float a~a = missing~a;\n" i i)))
                                                "}\n")))
    (check-true (exn:zgal? e))
    (when (exn:zgal? e)
      (check-equal? (exn:zgal-where e) (loc "check/shaders/big.comp" 2 1))
      (check-regexp-match #rx"glslc reports 3000 errors in all" (exn-message e))))

  (test-case "What naga can't translate is reported at the shader's declaration, as not-on-web"
    ;; A struct of the file enables 8-bit integers, which the body then uses
    (define e (translation-error "(struct P (a uint8))\n(compute-material C\n  (shaders \"shaders/small.comp\")\n  (workgroup 1) (bindings (sums (buffer-array float read-write))))"
                                 "void main() {\n    uint8_t v = uint8_t(gl_GlobalInvocationID.x);\n    sums()[0] = float(v);\n}\n"))
    (check-true (exn:zgal? e))
    (when (exn:zgal? e)
      (check-equal? (exn:zgal-id e) 'not-on-web)
      (check-equal? (exn:zgal-where e) (loc "check/c.zgal" 3 12))
      (check-regexp-match #rx"^naga can't translate small.comp to WGSL: .*[(]WGSL has no 8- and 16-bit integers[)]$" (exn-message e))
      (check-false (string-contains? (exn-message e) "\n"))))

  ;; text compiled for the web, the GLSL bodies from `bodies` (a path and its text each; paths are
  ;; the file's, check/c.zgal, so shaders are under check/)
  (define (compile-web text bodies #:toolchain [tools toolchain])
    (define-values (_prog outputs)
      (compile-zgal "check/c.zgal" #:text text #:native? #f #:web-dir "web" #:web-toolchain tools
                    #:read-body (λ (path) (cond [(assoc (path->string (find-relative-path (path->complete-path "check") (path->complete-path path))) bodies) => cdr] [else #f]))))
    outputs)
  (define (web-compile-error text bodies)
    (with-handlers ([exn:zgal? values]) (compile-web text bodies) #f))
  (define (output-named outputs name)
    (output-text (findf (λ (o) (string=? (path->string (file-name-from-path (output-path o))) name)) outputs)))

  (test-case "A material with GLSL bodies is described for the web by its bindings as the GLSL declares them, one each"
    (define outputs
      (compile-web (string-append
                    "(compute-material Shade (shaders \"shaders/shade.comp\") (workgroup 1)\n"
                    "  (bindings (shadow texture) (shadowSampler sampler) (unused (buffer-array float read)) (stamps (storage-image write)) (results (buffer-array float write)))\n"
                    "  (push (reference float)))\n"
                    "(graphics-material Show (shaders \"shaders/show.vert\" \"shaders/show.frag\")\n"
                    "  (bindings (offsets (buffer-array vec2 read)) (picture texture #:stages (fragment)) (pictureSampler sampler))\n"
                    "  (varyings (uv vec2)) (outputs (color vec4)))")
                   '(("shaders/shade.comp" . "void main() {\n    results()[0] = textureLod(sampler2DShadow(shadow(), shadowSampler()), vec3(0.5, 0.5, reference()), 0.0);\n    imageStore(stamps(), ivec2(0), vec4(1.0));\n}\n")
                     ("shaders/show.vert" . "void main() {\n    uv = offsets()[gl_VertexIndex];\n    gl_Position = vec4(uv, 0.0, 1.0);\n}\n")
                     ("shaders/show.frag" . "void main() {\n    color = texture(sampler2D(picture(), pictureSampler()), uv);\n}\n"))))
    (define ts (output-named outputs "c.zgal.ts"))
    (define (has line) (check-true (string-contains? ts (string-append line "\n")) line))
    (with-check-info (['ts ts])
      (has "  glsl: true,")
      ;; Compared: a depth texture and a comparison sampler; declared but unused: no stages
      (has "    { name: 'shadow', resource: 'shadow', kind: 'texture', access: null, stages: ['compute'], depth: true },")
      (has "    { name: 'shadowSampler', resource: 'shadowSampler', kind: 'comparison-sampler', access: null, stages: ['compute'], depth: true },")
      (has "    { name: 'unused', resource: 'unused', kind: 'buffer-array', access: 'read', stages: [], depth: false },")
      (has "    { name: 'stamps', resource: 'stamps', kind: 'storage-image', access: 'write', stages: ['compute'], depth: false },")
      ;; The push constants after the bindings, as the GLSL's uniform block
      (has "  push: { binding: 5, size: 16, fields: {")
      ;; Sampled: no variants; each binding visible to the stages using it
      (has "    { name: 'offsets', resource: 'offsets', kind: 'buffer-array', access: 'read', stages: ['vertex'], depth: false },")
      (has "    { name: 'pictureSampler', resource: 'pictureSampler', kind: 'sampler', access: null, stages: ['fragment'], depth: false },")
      (check-false (string-contains? ts "zgalCompare_"))
      (check-false (string-contains? ts "zgalDepth_"))
      ;; The storage image's format is the bound image's
      (check-regexp-match #rx"texture_storage_2d<\\${formats\\['stamps'\\]},write>" ts))
    (check-regexp-match #rx"texture_storage_2d<rgba8unorm,write>" (output-named outputs "shade.comp.wgsl")))

  (test-case "What the web can't bind of a material with GLSL bodies is refused at the material"
    (define (check-refused text bodies where pattern)
      (define e (web-compile-error text bodies))
      (check-true (exn:zgal? e) text)
      (when (exn:zgal? e)
        (check-equal? (exn:zgal-id e) 'not-on-web)
        (check-equal? (exn:zgal-where e) where)
        (check-regexp-match pattern (exn-message e))))
    ;; Mixed with a ZGAL body
    (check-refused "(graphics-material G (shaders \"shaders/g.vert\") (outputs (color vec4))\n  (fragment (vec4 1.0)))"
                   '(("shaders/g.vert" . "void main() { gl_Position = vec4(0.0); }\n"))
                   (loc "check/c.zgal" 2 3) #rx"^G mixes GLSL and ZGAL bodies")
    ;; A texture (and sampler) compared in one stage and sampled in another: the first binding is reported
    (check-refused "(graphics-material G (shaders \"shaders/g.vert\" \"shaders/g.frag\")\n  (bindings (t texture) (s sampler)) (outputs (color vec4)))"
                   '(("shaders/g.vert" . "void main() { gl_Position = vec4(textureLod(sampler2DShadow(t(), s()), vec3(0.5), 0.0)); }\n")
                     ("shaders/g.frag" . "void main() { color = texture(sampler2D(t(), s()), vec2(0.5)); }\n"))
                   (loc "check/c.zgal" 2 13) #rx"^t is compared with depths in one of G's shaders and sampled in another.*G's bodies are GLSL.*write the bodies in ZGAL")
    ;; The native interface's descriptor indexing, at the body's line
    (check-refused "(compute-material C (shaders \"shaders/c.comp\") (workgroup 1))"
                   '(("shaders/c.comp" . "// The native interface\n#include \"bindless.glsl\"\nvoid main() {}\n"))
                   (loc "check/shaders/c.comp" 2 1) #rx"^c.comp includes bindless.glsl"))

  (test-case "Compiling GLSL bodies for the web without the tools is an error of the caller's"
    (check-exn #rx"material C has GLSL bodies, which the web needs glslc and naga for"
               (λ () (compile-web "(compute-material C (shaders \"shaders/c.comp\") (workgroup 1))"
                                  '(("shaders/c.comp" . "void main() {}\n")) #:toolchain #f))))

  (test-case "A tool path that isn't there is an error of the toolchain's, not of the program's"
    (define body '(("shaders/c.comp" . "void main() {}\n")))
    (for ([tools (list (web-toolchain "/nonexistent/glslc" (getenv "ZGAL_NAGA") '())
                       (web-toolchain (getenv "ZGAL_GLSLC") "/nonexistent/naga" '()))])
      (define e (with-handlers ([exn:fail? values])
                  (compile-web "(compute-material C (shaders \"shaders/c.comp\") (workgroup 1))" body #:toolchain tools)))
      (check-true (exn:fail? e))
      (check-false (exn:zgal? e))
      (check-regexp-match #rx"^web-glsl: /nonexistent/(glslc|naga) isn't installed" (exn-message e))))

  (test-case "The command line translates GLSL bodies for the web with the naga it's given"
    (with-temporary-directory (directory)
      (make-directory (build-path directory "shaders"))
      (define source (build-path directory "c.zgal"))
      (call-with-output-file source (λ (out) (write-string "(compute-material C (shaders \"shaders/c.comp\") (workgroup 1) (bindings (sums (buffer-array float write))))\n" out)))
      ;; Found beside the .zgal file, as natively
      (call-with-output-file (build-path directory "shaders" "c.comp") (λ (out) (write-string "#include \"two.glsl\"\nvoid main() { sums()[0] = TWO; }\n" out)))
      (call-with-output-file (build-path directory "shaders" "two.glsl") (λ (out) (write-string "#define TWO 2.0\n" out)))
      (define-values (status text)
        (parameterize ([current-directory directory])
          (run (find-system-path 'exec-file) (path->string zgal.rkt) "--web-out" "web"
               "--glslc" (getenv "ZGAL_GLSLC") "--naga" (getenv "ZGAL_NAGA") "c.zgal")))
      (with-check-info (['output text])
        (check-equal? status 0)
        (check-regexp-match #rx"zgal_sums[.]data\\[0[iu]?\\] = 2(f|[.]0)?;" (file->string (build-path directory "web" "c.comp.wgsl")))))))

(define (cxx-tests)
  (define (compile-cxx directory source)
    (define file (build-path directory "test.cpp"))
    (call-with-output-file file (λ (out) (void (write-string source out))) #:exists 'truncate/replace)
    (apply run (getenv "ZGAL_CXX")
           (append (flags "ZGAL_CXXFLAGS")
                   (list (format "-I~a" (path->string directory)) "-fsyntax-only" (path->string file)))))

  (test-case "The generated main compiles"
    (with-temporary-directory (directory)
      (write-outputs (golden-outputs) directory)
      (define-values (status text)
        (apply run (getenv "ZGAL_CXX")
               (append (flags "ZGAL_CXXFLAGS")
                       (list (format "-I~a" (path->string directory)) "-fsyntax-only"
                             (path->string (build-path directory "features.zgal.main.cpp"))))))
      (with-check-info (['compiler text])
        (check-equal? status 0))))

  (test-case "The generated header compiles, and materials are abstract only if they push constants"
    (with-temporary-directory (directory)
      (write-outputs (golden-outputs) directory)
      (define-values (status text)
        (compile-cxx directory #<<CPP
#include "features.zgal.h"
#include <type_traits>

using namespace features;

struct SurfaceMaterial : Surface {
    using Surface::Surface;
    void writePush(uint32_t, Push& push) override { push.object = 1; push.tint = glm::vec3(1.0f); }
};
struct AccumulateMaterial : Accumulate {
    using Accumulate::Accumulate;
    void update(uint32_t, VkExtent2D) override {}
    glm::vec3 getDispatchDimensions() override { return workgroupsFor(100, 100); }
};
static_assert(std::is_abstract_v<Surface>);
static_assert(!std::is_abstract_v<SurfaceMaterial>);
static_assert(!std::is_abstract_v<AccumulateMaterial>);
static_assert(!std::is_abstract_v<Plain>);
static_assert(Surface::kColorOutputs == 2 && Plain::kColorOutputs == 1);
static_assert(Accumulate::kWorkgroupSizeX == 8 && Accumulate::kWorkgroupSizeY == 8 && Accumulate::kWorkgroupSizeZ == 1);
static_assert(Surface::kPushOffset == 20 && Surface::kPushEnd == 44);
static_assert(sizeof(Lights) == 144 && offsetof(Lights, lights) == 16);
CPP
                     ))
      (with-check-info (['compiler text])
        (check-equal? status 0))))

  (test-case "Files' declarations are in their own namespaces, and C++ errors in them point at the .zgal file"
    (with-temporary-directory (directory)
      ;; Two files declaring the same struct can be included together
      (for ([name '("a" "b")])
        (write-outputs (compile-text "\n(struct Shared\n  (x float))" #:source (format "check/~a.zgal" name))
                       directory))
      (define-values (status text)
        (compile-cxx directory "#include \"a.zgal.h\"\n#include \"b.zgal.h\"\nstatic_assert(sizeof(a::Shared) == sizeof(b::Shared));\n"))
      (with-check-info (['compiler text])
        (check-equal? status 0))
      ;; A C++ declaration clashing with a generated one is reported at the .zgal line
      (define-values (clash-status clash-text)
        (compile-cxx directory "namespace a { struct Shared {}; }\n#include \"a.zgal.h\"\n"))
      (check-not-equal? clash-status 0)
      (check-regexp-match #rx"check/a[.]zgal:2:[0-9]+: error: redefinition of 'Shared'" clash-text))))

(define (command-line-tests)
  (test-case "The command line reports errors in the compiler's format"
    (with-temporary-directory (directory)
      (define source (build-path directory "bad.zgal"))
      (call-with-output-file source (λ (out) (write-string "(struct A\n  (x float3))" out)))
      (define-values (status text) (run (find-system-path 'exec-file) (path->string zgal.rkt) "--check" (path->string source)))
      (check-equal? status 1)
      (check-equal? text (format "~a:2:6: error: unknown type float3 [unknown-type]\n" (path->string source)))))

  (test-case "The command line writes the outputs and a depfile, and leaves unchanged files alone"
    (with-temporary-directory (directory)
      (define source (build-path directory "ok.zgal"))
      (call-with-output-file source
        (λ (out) (write-string "(compute-material C (shaders \"c.comp\") (workgroup 1))" out)))
      (call-with-output-file (build-path directory "c.comp") (λ (out) (write-string "void main() {}\n" out)))
      (define out (build-path directory "out"))
      (define depfile (build-path out "ok.d"))
      (define (generate)
        (run (find-system-path 'exec-file) (path->string zgal.rkt)
             "--cpp-out" (path->string out) "--glsl-out" (path->string (build-path out "shaders"))
             "--depfile" (path->string depfile) "--dep-target" "ok.stamp" (path->string source)))
      (define-values (status text) (generate))
      (check-equal? status 0 text)
      (check-true (file-exists? (build-path out "ok.zgal.h")))
      (check-true (file-exists? (build-path out "shaders" "c.comp")))
      (check-equal? (file->string depfile)
                    (format "ok.stamp: ~a ~a\n~a:\n" source (build-path directory "c.comp") (build-path directory "c.comp")))
      (define before (file-or-directory-modify-seconds (build-path out "ok.zgal.h")))
      (sleep 1.1)
      (generate)
      (check-equal? (file-or-directory-modify-seconds (build-path out "ok.zgal.h")) before))))

(module+ test
  (when-tool "ZGAL_GLSLC" glslc-tests)
  (when-tool "ZGAL_CXX" cxx-tests)
  (when-tool "ZGAL_NODE" dawn-tests)
  (when-tool "ZGAL_NAGA" naga-tests)
  (command-line-tests))
