#lang racket/base
;; The compiler's output for golden/features.zgal, and for the web for golden/web.zgal, compared
;; with golden/expected. When the output
;; changes on purpose, regenerate the expected files with
;;     ZGAL_UPDATE_GOLDEN=1 raco test dsl/tests/golden-test.rkt
;; and review the difference like any other change.

(require racket/file
         racket/list
         racket/path
         rackunit
         "../compile.rkt"
         "util.rkt")

(provide golden-outputs
         golden-web-outputs
         golden-source)

(define golden-directory (build-path tests-directory "golden"))
(define expected-directory (build-path golden-directory "expected"))
;; Paths as the Makefile would give them, so the output doesn't depend on where the tests run
(define golden-source "dsl/tests/golden/features.zgal")
(define golden-web-source "dsl/tests/golden/web.zgal")

(define (read-body path)
  (define file (build-path tests-directory (path->string (find-relative-path "dsl/tests" path))))
  (and (file-exists? file) (file->string file)))

;; The outputs for features.zgal, written to build/generated
(define (golden-outputs)
  (define-values (_prog outputs)
    (compile-zgal golden-source
                  #:text (file->string (build-path golden-directory "features.zgal"))
                  #:read-body read-body
                  #:header-dir "build/generated"
                  #:shader-dir "build/generated/shaders"))
  outputs)

;; The web outputs for web.zgal, written to build/generated/web
(define (golden-web-outputs)
  (define-values (_prog outputs)
    (compile-zgal golden-web-source
                  #:text (file->string (build-path golden-directory "web.zgal"))
                  #:native? #f
                  #:web-dir "build/generated/web"))
  outputs)

(define (expected-file o)
  (build-path expected-directory (file-name-from-path (output-path o))))

(module+ test
  (define outputs (append (golden-outputs) (golden-web-outputs)))
  (cond
    [(getenv "ZGAL_UPDATE_GOLDEN")
     (make-directory* expected-directory)
     (for ([o (in-list outputs)])
       (call-with-output-file (expected-file o) (λ (out) (void (write-string (output-text o) out)))
         #:exists 'truncate/replace))
     (printf "Updated ~a expected files\n" (length outputs))]
    [else
     (test-case "The generated header and shaders (GLSL and WGSL) match the expected ones"
       (check-equal? (sort (map (λ (o) (path->string (file-name-from-path (output-path o)))) outputs) string<?)
                     (sort (map path->string (directory-list expected-directory)) string<?)
                     "the same files")
       (for ([o (in-list outputs)])
         (with-check-info (['file (output-path o)])
           (define expected (expected-file o))
           (check-true (file-exists? expected))
           (when (file-exists? expected)
             (check-equal? (output-text o) (file->string expected))))))]))
