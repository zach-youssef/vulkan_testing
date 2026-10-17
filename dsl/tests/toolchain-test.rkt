#lang racket/base
;; Generated code through the real tools: glslc and the C++ compiler, with the build's flags (the
;; Makefile's test-dsl target passes them in ZGAL_GLSLC, ZGAL_GLSLC_FLAGS, ZGAL_CXX and
;; ZGAL_CXXFLAGS), and the command line

(require racket/file
         racket/list
         racket/path
         racket/port
         racket/runtime-path
         racket/string
         racket/system
         rackunit
         "../compile.rkt"
         "golden-test.rkt"
         "util.rkt")

(define-runtime-path zgal.rkt "../zgal.rkt")

;; Runs a program, returning its exit code and what it wrote to stdout and stderr
(define (run program . arguments)
  (define-values (process out in err)
    (apply subprocess #f #f #f (find-executable-path* program) arguments))
  (close-output-port in)
  (define output (port->string out))
  (define errors (port->string err))
  (subprocess-wait process)
  (close-input-port out)
  (close-input-port err)
  (values (subprocess-status process) (string-append output errors)))

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
      (for ([o (in-list (rest outputs))])
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

(define (cxx-tests)
  (define (compile-cxx directory source)
    (define file (build-path directory "test.cpp"))
    (call-with-output-file file (λ (out) (void (write-string source out))) #:exists 'truncate/replace)
    (apply run (getenv "ZGAL_CXX")
           (append (flags "ZGAL_CXXFLAGS")
                   (list (format "-I~a" (path->string directory)) "-fsyntax-only" (path->string file)))))

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
  (command-line-tests))
