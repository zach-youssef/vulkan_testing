#lang racket/base
;; The ZGAL compiler's command line. See dsl/docs (make docs) for the language.
;;
;;   racket dsl/zgal.rkt [--cpp-out DIR] [--glsl-out DIR] [--depfile FILE --dep-target TARGET] FILE.zgal
;;   racket dsl/zgal.rkt --web-out DIR FILE.zgal
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

;; A Makefile rule making `target` depend on the .zgal file, the files it imports and its shader
;; bodies, with an empty rule per one of those so deleting it doesn't break the build (as gcc -MP
;; does)
(define (dependency-rule target source prog)
  (define bodies
    (remove-duplicates
     (append
      (program-imports prog)
      (program-glsl-files prog)
      (for*/list ([m (in-list (program-materials prog))] [s (in-list (material-shaders m))] #:when (shader-path s))
        (shader-path s)))))
  (string-append
   (format "~a: ~a\n" target (string-join (cons source bodies) " "))
   (string-append* (for/list ([b (in-list bodies)]) (format "~a:\n" b)))))

;; Removes the shaders in directory (which holds one file's) that weren't just generated: those of
;; materials since renamed or removed. pattern: matches shaders' file names.
(define (remove-stale-shaders directory generated [pattern #rx"[.](vert|frag|comp)$"])
  (define kept (for/list ([p (in-list generated)]) (path->complete-path (simplify-path p))))
  (when (directory-exists? directory)
    (for ([p (in-list (directory-list directory #:build? #t))]
          #:when (regexp-match? pattern (path->string p))
          #:unless (member (path->complete-path (simplify-path p)) kept))
      (delete-file p))))

(module+ main
  (define cpp-out #f)
  (define glsl-out #f)
  (define web-out #f)
  (define depfile #f)
  (define dep-target #f)
  (define check-only? #f)
  (define source
    (command-line
     #:program "zgal"
     #:once-each
     [("--cpp-out") dir "Write the C++ header to DIR/<file>.h (and a program's main to DIR/<file>.main.cpp)" (set! cpp-out dir)]
     [("--glsl-out") dir "Write each shader to DIR/<shader name>, removing others there" (set! glsl-out dir)]
     [("--web-out") dir "Write the TypeScript module (WebGPU) to DIR/<file>.ts (a program's main to DIR/<file>.main.ts, and its page to DIR/<file>.html) and each shader's WGSL to DIR/<shader name>.wgsl, removing other WGSL there" (set! web-out dir)]
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
    ;; Only for the web, the native outputs aren't generated (so GLSL bodies needn't exist)
    (define native? (or cpp-out glsl-out depfile (not web-out)))
    (define-values (prog all-outputs)
      (compile-zgal source
                    #:header-dir (or cpp-out ".")
                    #:shader-dir (or glsl-out ".")
                    #:native? native?
                    #:web-dir web-out
                    #:check-only? check-only?))
    (define-values (web outputs) (partition (λ (o) (regexp-match? #rx"[.](wgsl|ts|html)$" (output-path o))) all-outputs))
    (when (and web-out (not check-only?))
      (for ([o (in-list web)]) (write-if-changed (output-path o) (output-text o)))
      (remove-stale-shaders web-out (map output-path web) #rx"[.]wgsl$"))
    (unless (or check-only? (not native?))
      (define header (first outputs))
      (define-values (cpp shaders)
        (partition (λ (o) (regexp-match? #rx"[.](h|main[.]cpp)$" (output-path o))) outputs))
      (when cpp-out
        (for ([o (in-list cpp)]) (write-if-changed (output-path o) (output-text o))))
      (when glsl-out
        (for ([o (in-list shaders)]) (write-if-changed (output-path o) (output-text o)))
        (remove-stale-shaders glsl-out (map output-path shaders)))
      (when depfile
        (write-if-changed depfile (dependency-rule (or dep-target (output-path header)) source prog))))))
