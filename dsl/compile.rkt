#lang racket/base
;; Compiling a .zgal file: reading and checking it, then generating its C++ header and shaders.
;; The command line (zgal.rkt), the tests and the documentation all compile through this.

(require racket/file
         racket/path
         racket/promise
         "private/ast.rkt"
         "private/cpp.rkt"
         "private/cpp-main.rkt"
         "private/errors.rkt"
         "private/glsl.rkt"
         "private/parse.rkt"
         "private/ts.rkt"
         "private/ts-main.rkt"
         "private/web-glsl.rkt"
         "private/wgsl.rkt")

(provide compile-zgal
         (struct-out output)
         (struct-out exn:zgal)
         (struct-out web-toolchain)
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
;; With a web-dir, the outputs for the web follow: the TypeScript module (<file>.ts), for a program
;; its main (<file>.main.ts) and with a window its page (<file>.html), then a WGSL module per
;; material shader and one per library kernel's variant without subgroups (which the TypeScript also
;; holds). Materials with GLSL bodies, or calling GLSL
;; functions (extern-glsl), are translated for the web through GLSL with web-toolchain (a
;; web-toolchain: glslc and naga), which only those need.
;; Raises exn:zgal for errors in the program.
(define (compile-zgal source
                      #:text [text #f]
                      #:read-body [read-body read-file-or-false]
                      #:header-dir [header-dir "."]
                      #:shader-dir [shader-dir "."]
                      #:native? [native? #t]
                      #:web-dir [web-dir #f]
                      #:web-toolchain [web-toolchain #f]
                      #:check-only? [check-only? #f])
  (define forms (read-forms source (open-input-string (or text (file->string source)))))
  (define prog (parse-program forms source))
  ;; A GLSL body's text
  (define (body-of s)
    (or (read-body (shader-path s))
        (zgal-error 'shader-not-found (shader-loc s) "shader body ~a doesn't exist" (shader-path s))))
  ;; The texts of the files of the GLSL functions it declares, by path (read once, when a shader
  ;; needs them)
  (define glsl-files
    (delay
      (for/hash ([path (in-list (program-glsl-files prog))])
        (values path
                (or (read-body path)
                    (zgal-error 'shader-not-found
                                (for/first ([d (in-list (program-functions prog))]
                                            #:when (and (glsl-function-decl? d) (equal? (glsl-function-decl-path d) path)))
                                  (function-decl-loc d))
                                "GLSL file ~a doesn't exist" path))))))
  (define web
    (if (and web-dir (not check-only?))
        (let ([glsl (for/hash ([m (in-list (program-materials prog))] #:when (glsl-material? prog m))
                      (unless web-toolchain
                        (error 'compile-zgal "material ~a has GLSL bodies or calls GLSL functions, which the web needs glslc and naga for: give a #:web-toolchain"
                               (material-name m)))
                      (values (material-name m)
                              (translate-web-material web-toolchain prog m body-of #:glsl-files (force glsl-files))))])
          (append
           (list (output (join web-dir (typescript-file-name source)) (emit-typescript prog #:glsl-materials glsl)))
           (let ([m (program-main prog)])
             (cond
               [(not m) '()]
               [else (list (output (join web-dir (ts-main-file-name source)) (emit-ts-main prog m))
                           (output (join web-dir (ts-page-file-name source)) (emit-ts-page prog m)))]))
           ;; A storage image's format is emit-wgsl's default here (rgba8unorm); the TypeScript's
           ;; modules take the bound image's
           (for*/list ([m (in-list (program-materials prog))]
                       [s (in-list (material-shaders m))])
             (define translated (hash-ref glsl (material-name m) #f))
             (output (join web-dir (wgsl-shader-file-name s))
                     (if translated
                         (web-material-module translated (shader-stage s) #:storage-format (λ (b) "rgba8unorm"))
                         (emit-wgsl prog m s))))
           ;; A library kernel's variant without subgroups (the description's withoutSubgroups),
           ;; which devices without the feature (Firefox) run: written so tools check it too
           (for*/list ([m (in-list (program-materials prog))]
                       [fallback (in-value (hash-ref (program-fallbacks prog) (material-name m) #f))]
                       #:when fallback
                       [s (in-list (material-shaders fallback))])
             (output (join web-dir (wgsl-fallback-file-name s)) (emit-wgsl prog fallback s)))))
        '()))
  (cond
    [check-only? (values prog '())]
    [(not native?) (values prog web)]
    [else
     (define header-path (join header-dir (header-file-name source)))
     (define shaders
       (for*/list ([m (in-list (program-materials prog))]
                   [s (in-list (material-shaders m))])
         ;; GLSL bodies are files; ZGAL ones are in the program
         (define body (and (shader-path s) (body-of s)))
         (define out-path (join shader-dir (shader-name s)))
         (output out-path (emit-shader prog m s body out-path #:interface 'bindless #:glsl-files (force glsl-files)))))
     (define main
       (if (program-main prog)
           (let ([path (join header-dir (main-file-name source))])
             (list (output path (emit-main prog (program-main prog) (header-file-name source) path))))
           '()))
     (values prog (append (list (output header-path (emit-header prog header-path))) main shaders web))]))
