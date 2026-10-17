#lang racket/base
;; Helpers for reading forms: their shape, the names they declare, and namespaces of names

(require racket/match
         racket/string
         "errors.rkt"
         "names.rkt")

(provide show
         head-symbol
         expect-list
         camel-case
         parse-name
         make-namespace)

(define (show stx) (format "~s" (syntax->datum stx)))

(define (head-symbol stx)
  (define items (syntax->list stx))
  (and items (pair? items) (symbol? (syntax-e (car items))) (syntax-e (car items))))

(define (expect-list stx what)
  (or (syntax->list stx)
      (zgal-error 'malformed stx "expected ~a, found ~a" what (show stx))))

(define (camel-case s)
  (match (string-split s "-")
    [(cons first rest)
     (apply string-append first
            (for/list ([part (in-list rest)] #:unless (string=? part ""))
              (string-append (string-upcase (substring part 0 1)) (substring part 1))))]
    [_ s]))

;; A declared name: a symbol that is a valid, unreserved identifier. Returns it as a string.
(define (parse-name stx what)
  (define d (syntax-e stx))
  (unless (symbol? d)
    (zgal-error 'malformed stx "expected ~a, found ~a" what (show stx)))
  (define s (symbol->string d))
  (unless (valid-identifier? s)
    (if (string-contains? s "-")
        (zgal-error 'bad-identifier stx
                    "~a isn't a valid name: names become GLSL and C++ identifiers, so write ~a"
                    s (camel-case s))
        (zgal-error 'bad-identifier stx
                    "~a isn't a valid name: use letters, digits and underscores, starting with a letter (and no __)"
                    s)))
  (define reason (reserved-name-reason s))
  (when reason
    (zgal-error 'reserved-name stx "~a can't be used as a name: it ~a" s reason))
  s)

;; Tracks names in one namespace, reporting the second declaration of a name
(define (make-namespace description)
  (define seen (make-hash))
  (λ (name stx [what "it"])
    (define previous (hash-ref seen name #f))
    (when previous
      (zgal-error 'duplicate-name stx "~a is already declared ~a (line ~a)"
                  name description (loc-line previous)))
    (hash-set! seen name (stx->loc stx))))
