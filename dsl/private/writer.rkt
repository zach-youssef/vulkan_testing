#lang racket/base
;; Writes generated source line by line, mapping lines to where they came from with #line
;; directives (understood by C++ compilers, and by glslc with GL_GOOGLE_cpp_style_line_directive).
;; A line emitted with a location is reported by the compiler at that location; one emitted without
;; is reported at its own line in the generated file.

(require racket/list
         racket/string
         "errors.rkt")

(provide make-writer
         emit!
         writer-text)

;; path: the generated file's path, as compilers will be given it
;; next: the physical number of the next line
;; mapped: #f when lines are numbered as themselves, else (cons source line) the next line would
;; be reported at without a new directive
(struct writer (path [lines #:mutable] [next #:mutable] [mapped #:mutable]))

(define (make-writer path) (writer path '() 1 #f))

(define (put! w text)
  (set-writer-lines! w (cons text (writer-lines w)))
  (set-writer-next! w (add1 (writer-next w))))

(define (directive line source) (format "#line ~a ~s" line source))

;; Emits text (one or more lines). at: a loc, or #f for generated lines.
(define (emit! w text #:at [at #f])
  (for ([line (in-list (regexp-split #rx"\n" text))])
    (cond
      [at
       (define wanted (cons (loc-source at) (loc-line at)))
       (unless (equal? (writer-mapped w) wanted)
         (put! w (directive (loc-line at) (loc-source at))))
       (put! w line)
       ;; Later lines with the same location repeat the directive: they're the same declaration
       (set-writer-mapped! w (cons (loc-source at) (add1 (loc-line at))))]
      [else
       (when (writer-mapped w)
         ;; The directive is on the next line, so the line after it is numbered one more
         (put! w (directive (add1 (writer-next w)) (writer-path w)))
         (set-writer-mapped! w #f))
       (put! w line)])))

(define (writer-text w)
  (string-append (string-join (reverse (writer-lines w)) "\n") "\n"))
