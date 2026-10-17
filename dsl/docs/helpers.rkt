#lang racket/base
;; Executable documentation: examples are compiled while the documentation builds, so what it shows
;; is what the compiler does. An example that fails unexpectedly (or doesn't fail when it should)
;; fails the build.

(require racket/file
         racket/list
         racket/match
         racket/path
         racket/runtime-path
         racket/string
         scribble/base
         scribble/core
         scribble/manual
         "../compile.rkt"
         "../private/ast.rkt"
         "../private/errors.rkt"
         "../private/builtins.rkt"
         "../private/graph.rkt"
         "../private/layout.rkt"
         "../private/parse.rkt")

(provide format-table
         repository-excerpt
         builtin-function-list
         stage-variable-table
         repository-file
         prose
         zgal-source
         zgal-example
         generated-file
         layout-table
         error-catalog-section
         file-excerpt
         library-modules
         push-limit)

(define-runtime-path repository "../..")

(define push-limit push-constant-limit)

;; Text with `code` spans, as the error catalog writes it
(define (prose text)
  (for/list ([part (in-list (regexp-split #rx"`" text))] [i (in-naturals)])
    (if (odd? i) (tt part) part)))

;; Shows .zgal source
(define (zgal-source text)
  (nested #:style 'code-inset (verbatim text)))

(define (shown-name o) (path->string (file-name-from-path (output-path o))))

;; Compiles `text` (as example.zgal, with `bodies`) and returns its outputs
(define (compile-example text bodies)
  (with-handlers ([exn:zgal? (λ (e) (error 'zgal-example "the example doesn't compile: ~a" (format-zgal-error e)))])
    (define-values (_prog outputs)
      (compile-zgal "example.zgal" #:text text #:read-body (λ (path) (hash-ref bodies path #f))
                    #:header-dir "generated" #:shader-dir "generated/shaders"))
    outputs))

;; Shows the source, then the generated files named in `show` (file names, e.g. "example.zgal.h"
;; or "blur.comp"), optionally only the lines from a line matching `from` to one matching `to`
(define (zgal-example text #:bodies [bodies (hash)] #:show [show '()] #:from [from #f] #:to [to #f])
  (define outputs (compile-example text bodies))
  (define by-name (for/hash ([o (in-list outputs)]) (values (shown-name o) o)))
  (list
   (zgal-source text)
   (for/list ([name (in-list show)])
     (define o (hash-ref by-name name (λ () (error 'zgal-example "no output named ~a" name))))
     (list (para (italic (format "Generated ~a~a:" name (if (or from to) " (excerpt)" ""))))
           (nested #:style 'code-inset (verbatim (excerpt (output-text o) from to)))))))

;; The lines of text from the first matching `from` through the next matching `to`
(define (excerpt text from to)
  (define lines (string-split text "\n" #:trim? #f))
  (define start (if from (or (index-where lines (λ (l) (regexp-match? from l))) (error 'excerpt "no line matches ~a" from)) 0))
  (define after (drop lines start))
  (define end (if to (add1 (or (index-where after (λ (l) (regexp-match? to l))) (error 'excerpt "no line matches ~a" to)))
                  (length after)))
  (string-join (take after end) "\n"))

;; A generated file of an example, alone
(define (generated-file text name #:bodies [bodies (hash)] #:from [from #f] #:to [to #f])
  (define o (or (for/first ([o (in-list (compile-example text bodies))] #:when (string=? (shown-name o) name)) o)
                (error 'generated-file "no output named ~a" name)))
  (nested #:style 'code-inset (verbatim (excerpt (output-text o) from to))))

;; A table of the offsets, sizes and alignments the compiler gives a struct's fields
(define (layout-table text struct-name)
  (define forms (read-forms "example.zgal" (open-input-string text)))
  (define prog (parse-program forms "example.zgal"))
  (define d (or (for/first ([d (in-list (program-structs prog))] #:when (string=? (struct-decl-name d) struct-name)) d)
                (error 'layout-table "no struct ~a" struct-name)))
  (define-values (placed _end) (place-fields (struct-decl-fields d) 0))
  (tabular
   #:style 'boxed
   #:sep (hspace 2)
   #:row-properties '(bottom-border ())
   (append
    (list (list (bold "Field") (bold "Type") (bold "Offset") (bold "Size") (bold "Alignment")))
    (for/list ([p (in-list placed)])
      (define t (field-type (placed-field p)))
      (list (tt (field-name (placed-field p))) (tt (type-name t))
            (number->string (placed-offset p)) (number->string (std430-size t)) (number->string (std430-align t))))
    (list (list (italic struct-name) "" "" (number->string (std430-size (struct-t d)))
                (number->string (std430-align (struct-t d))))))))

;; Every error in the catalog: its explanation, its example and the message the example gets
(define (error-catalog-section)
  (for/list ([k (in-list error-catalog)])
    (define e
      (with-handlers ([exn:zgal? values])
        (compile-zgal "example.zgal" #:text (error-kind-example k)
                      #:read-body (λ (path) (hash-ref (error-kind-bodies k) path #f)))
        (error 'error-catalog-section "the example of ~a compiles" (error-kind-id k))))
    (unless (eq? (exn:zgal-id e) (error-kind-id k))
      (error 'error-catalog-section "the example of ~a reports ~a" (error-kind-id k) (exn:zgal-id e)))
    (list
     (subsection #:tag (format "error-~a" (error-kind-id k)) #:style 'unnumbered
                 (tt (symbol->string (error-kind-id k))) ": " (error-kind-title k))
     (para (prose (error-kind-explanation k)))
     (for/list ([(path body) (in-hash (error-kind-bodies k))])
       (list (para (italic (format "With the shader body ~a:" path)))
             (nested #:style 'code-inset (verbatim body))))
     (zgal-source (error-kind-example k))
     (nested #:style 'code-inset (verbatim (format-zgal-error e))))))

;; The image formats graphs name, and their Vulkan formats
(define (format-table)
  (tabular
   #:style 'boxed
   #:sep (hspace 2)
   #:row-properties '(bottom-border ())
   (cons (list (bold "ZGAL") (bold "Vulkan"))
         (for/list ([f (in-list (sort (hash-keys image-formats) symbol<?))])
           (list (tt (symbol->string f)) (tt (hash-ref image-formats f)))))))

;; Lines of a repository file, from one matching `from` through one matching `to`, as a string
(define (repository-excerpt path #:from [from #f] #:to [to #f])
  (excerpt (string-trim (file->string (build-path repository path)) "\n" #:left? #f) from to))

;; The built-in functions, by name
(define (builtin-function-list)
  (para (add-between (for/list ([n (in-list (builtin-names))]) (tt (symbol->string n))) ", ")))

;; The stage variables: their ZGAL names, types and GLSL spellings
(define (stage-variable-table)
  (tabular
   #:style 'boxed
   #:sep (hspace 2)
   #:row-properties '(bottom-border ())
   (cons (list (bold "Stage") (bold "Name") (bold "Type") (bold "GLSL"))
         (for*/list ([stage (in-list '(vertex fragment compute))]
                     [entry (in-list (sort (hash->list (hash-ref stage-variables stage)) string<? #:key car))])
           (list (symbol->string stage) (tt (car entry)) (tt (type-name (third entry))) (tt (second entry)))))))

;; A file in the repository, as a string
(define (repository-file path)
  (file->string (build-path repository path)))

;; Lines of a file in the repository, from a line matching `from` through one matching `to`
;; Every module of the standard library (dsl/lib), as its file reads
(define (library-modules)
  (define directory (build-path repository "dsl" "lib"))
  (for/list ([f (in-list (sort (map path->string (directory-list directory)) string<?))]
             #:when (regexp-match? #px"[.]zgal$" f))
    (list (para (bold (tt (format "(import ~a)" (regexp-replace #px"[.]zgal$" f "")))))
          (file-excerpt (string-append "dsl/lib/" f)))))

(define (file-excerpt path #:from [from #f] #:to [to #f])
  (nested #:style 'code-inset
          (verbatim (excerpt (string-trim (file->string (build-path repository path)) "\n" #:left? #f) from to))))
