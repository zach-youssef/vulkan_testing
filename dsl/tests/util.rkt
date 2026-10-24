#lang racket/base
;; Helpers for the ZGAL compiler's tests

(require racket/file
         racket/runtime-path
         rackunit
         "../compile.rkt"
         "../private/errors.rkt")

(provide compile-text
         check-zgal-error
         zgal-error-of
         tests-directory)

(define-runtime-path tests-directory ".")

;; Compiles `text` as if it were the file at `source`, with shader bodies from the `bodies` hash
;; (path -> text). Returns the outputs.
(define (compile-text text
                      #:source [source "test.zgal"]
                      #:bodies [bodies (hash)]
                      #:header-dir [header-dir "out"]
                      #:shader-dir [shader-dir "out/shaders"]
                      #:web-dir [web-dir #f])
  (define-values (_prog outputs)
    (compile-zgal source #:text text #:read-body (λ (path) (hash-ref bodies path #f))
                  #:header-dir header-dir #:shader-dir shader-dir #:web-dir web-dir))
  outputs)

;; The exn:zgal compiling text raises (for the web with web?), or #f
(define (zgal-error-of text #:bodies [bodies (hash)] #:web? [web? #f])
  (with-handlers ([exn:zgal? values])
    (if web?
        (compile-zgal "test.zgal" #:text text #:read-body (λ (path) (hash-ref bodies path #f))
                      #:native? #f #:web-dir "out/web")
        (compile-text text #:bodies bodies))
    #f))

;; Checks compiling text reports error `id`, at line:column if given, with a message containing
;; each of `fragments`
(define-syntax-rule (check-zgal-error text id fragments ...)
  (let ([e (zgal-error-of text)])
    (with-check-info (['program text])
      (check-true (exn:zgal? e) "expected an error")
      (when (exn:zgal? e)
        (with-check-info (['message (format-zgal-error e)])
          (check-equal? (exn:zgal-id e) id)
          (for ([fragment (in-list (list fragments ...))])
            (check-true (regexp-match? (regexp-quote fragment) (exn-message e))
                        (format "message lacks ~s" fragment))))))))
