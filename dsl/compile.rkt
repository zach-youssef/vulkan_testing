#lang racket/base
;; Compiling a .zgal file: reading and checking it, then generating its C++ header and shaders.
;; The command line (zgal.rkt), the tests and the documentation all compile through this.

(require racket/file
         racket/path
         "private/ast.rkt"
         "private/cpp.rkt"
         "private/cpp-main.rkt"
         "private/errors.rkt"
         "private/glsl.rkt"
         "private/parse.rkt"
         "private/wgsl.rkt")

(provide compile-zgal
         (struct-out output)
         (struct-out exn:zgal)
         format-zgal-error
         header-file-name
         main-file-name)

;; A generated file: where it goes and its contents
(struct output (path text) #:transparent)

(define (read-file-or-false path)
  (and (file-exists? path) (file->string path)))

;; The header generated for a .zgal file: its file name plus .h, e.g. scene.zgal.h
(define (header-file-name source)
  (string-append (path->string (file-name-from-path source)) ".h"))

;; Its main, for a program with one: e.g. scene.zgal.main.cpp
(define (main-file-name source)
  (string-append (path->string (file-name-from-path source)) ".main.cpp"))

(define (join directory name)
  (path->string (simplify-path (build-path directory name) #f)))

;; Compiles the .zgal file at `source` (a path as given, which error messages and #line directives
;; repeat). text: its contents, or #f to read the file. read-body: reads a shader body's path,
;; returning #f if it doesn't exist. header-dir, shader-dir: where the outputs go. With check-only?,
;; stops after checking (so shader bodies aren't read).
;;
;; With native?, returns the program and the outputs for Vulkan: the header, the main
;; (<file>.main.cpp, beside the header) for a program with one, then a shader per material shader.
;; With a web-dir, the outputs for the web follow: a WGSL module per material shader. Raises
;; exn:zgal for errors in the program.
(define (compile-zgal source
                      #:text [text #f]
                      #:read-body [read-body read-file-or-false]
                      #:header-dir [header-dir "."]
                      #:shader-dir [shader-dir "."]
                      #:native? [native? #t]
                      #:web-dir [web-dir #f]
                      #:check-only? [check-only? #f])
  (define forms (read-forms source (open-input-string (or text (file->string source)))))
  (define prog (parse-program forms source))
  (define web
    (if (and web-dir (not check-only?))
        (for*/list ([m (in-list (program-materials prog))]
                    [s (in-list (material-shaders m))])
          (output (join web-dir (wgsl-shader-file-name s)) (emit-wgsl prog m s)))
        '()))
  (cond
    [check-only? (values prog '())]
    [(not native?) (values prog web)]
    [else
     (define header-path (join header-dir (header-file-name source)))
     ;; The files of the GLSL functions it declares
     (define glsl-files
       (for/hash ([path (in-list (program-glsl-files prog))])
         (values path
                 (or (read-body path)
                     (zgal-error 'shader-not-found
                                 (for/first ([d (in-list (program-functions prog))]
                                             #:when (and (glsl-function-decl? d) (equal? (glsl-function-decl-path d) path)))
                                   (function-decl-loc d))
                                 "GLSL file ~a doesn't exist" path)))))
     (define shaders
       (for*/list ([m (in-list (program-materials prog))]
                   [s (in-list (material-shaders m))])
         ;; GLSL bodies are files; ZGAL ones are in the program
         (define body
           (and (shader-path s)
                (or (read-body (shader-path s))
                    (zgal-error 'shader-not-found (shader-loc s) "shader body ~a doesn't exist" (shader-path s)))))
         (define out-path (join shader-dir (shader-name s)))
         (output out-path (emit-shader prog m s body out-path #:glsl-files glsl-files))))
     (define main
       (if (program-main prog)
           (let ([path (join header-dir (main-file-name source))])
             (list (output path (emit-main prog (program-main prog) (header-file-name source) path))))
           '()))
     (values prog (append (list (output header-path (emit-header prog header-path))) main shaders web))]))
