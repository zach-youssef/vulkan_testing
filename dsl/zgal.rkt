#lang racket/base
;; The ZGAL compiler's command line. See dsl/docs (make docs) for the language.
;;
;;   racket dsl/zgal.rkt [--cpp-out DIR] [--glsl-out DIR] [--depfile FILE --dep-target TARGET] FILE.zgal
;;   racket dsl/zgal.rkt --check FILE.zgal

(require racket/cmdline
         racket/file
         racket/list
         racket/string
         "compile.rkt"
         "private/ast.rkt")

;; Writes a file unless it already has that content, so unchanged outputs keep their timestamps
(define (write-if-changed path text)
  (unless (and (file-exists? path) (string=? (file->string path) text))
    (define-values (directory _name _dir?) (split-path path))
    (when (path? directory) (make-directory* directory))
    (call-with-output-file path (λ (out) (void (write-string text out))) #:exists 'truncate/replace)))

;; A Makefile rule making `target` depend on the .zgal file and its shader bodies, with an empty
;; rule per body so deleting one doesn't break the build (as gcc -MP does)
(define (dependency-rule target source prog)
  (define bodies
    (remove-duplicates
     (for*/list ([m (in-list (program-materials prog))] [s (in-list (material-shaders m))] #:when (shader-path s))
       (shader-path s))))
  (string-append
   (format "~a: ~a\n" target (string-join (cons source bodies) " "))
   (string-append* (for/list ([b (in-list bodies)]) (format "~a:\n" b)))))

(module+ main
  (define cpp-out #f)
  (define glsl-out #f)
  (define depfile #f)
  (define dep-target #f)
  (define check-only? #f)
  (define source
    (command-line
     #:program "zgal"
     #:once-each
     [("--cpp-out") dir "Write the C++ header to DIR/<file>.h" (set! cpp-out dir)]
     [("--glsl-out") dir "Write each shader to DIR/<shader name>" (set! glsl-out dir)]
     [("--depfile") file "Write a Makefile rule listing the inputs to FILE" (set! depfile file)]
     [("--dep-target") target "The target of the --depfile rule (default: the header)" (set! dep-target target)]
     [("--check") "Only check the file" (set! check-only? #t)]
     #:args (file) file))

  (unless (file-exists? source)
    (eprintf "zgal: ~a doesn't exist\n" source)
    (exit 2))
  (with-handlers ([exn:zgal? (λ (e)
                                (eprintf "~a\n" (format-zgal-error e))
                                (exit 1))])
    (define-values (prog outputs)
      (compile-zgal source
                    #:header-dir (or cpp-out ".")
                    #:shader-dir (or glsl-out ".")
                    #:check-only? check-only?))
    (unless check-only?
      (define header (first outputs))
      (when cpp-out (write-if-changed (output-path header) (output-text header)))
      (when glsl-out
        (for ([o (in-list (rest outputs))]) (write-if-changed (output-path o) (output-text o))))
      (when depfile
        (write-if-changed depfile (dependency-rule (or dep-target (output-path header)) source prog))))))
